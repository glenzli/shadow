#include "../src/proxy/warm_edit_gpu.hpp"
#include "scoped_environment.hpp"
#include "warm_edit_gpu_contract/warm_edit_gpu_parity_fixture.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

namespace image = shadow::image;
namespace fixture = image::warm_edit_gpu_contract::parity_fixture;
using Session = image::detail::WarmEditGpuSession;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::array<image::AdjustmentNode, 4> core_nodes() {
    return {{
        {.node_id = "wb",
         .parameters = image::RgbWhiteBalanceAdjustment{.temperature = .24, .tint = -.13}},
        {.node_id = "exposure", .parameters = image::ExposureAdjustment{.stops = .37}},
        {.node_id = "contrast",
         .parameters = image::ContrastAdjustment{.factor = 1.42, .pivot = .18}},
        {.node_id = "saturation", .parameters = image::SaturationAdjustment{.factor = .76}},
    }};
}

void compare(
    const Session::RenderAttempt& actual,
    const image::FloatRgbImage& expected,
    std::uint32_t origin_x = 0,
    std::uint32_t origin_y = 0
) {
    require(
        actual.status == Session::RenderStatus::completed && actual.output.has_value(),
        "CUDA render must complete: " + actual.diagnostic
    );
    require(
        actual.output->backend == image::detail::WarmEditGpuBackend::cuda,
        "GPU result must identify CUDA"
    );
    require(actual.output->analyzed_linear.has_value(), "Settled output must retain linear pixels");
    require(
        actual.output->analyzed_linear->row_stride_bytes
                == std::size_t(expected.dimensions.width) * 3U * sizeof(float)
            && actual.output->analyzed_linear->samples.size()
                   == std::size_t(expected.dimensions.pixel_count()) * 3U,
        "CUDA settled analysis must pack visible rows without source padding"
    );
    double error = 0;
    const bool linear_matches =
        fixture::linear_close(*actual.output->analyzed_linear, expected, error);
    require(linear_matches, "Linear CPU/CUDA mismatch: " + std::to_string(error));
    const auto display = image::render_linear_srgb_to_display_srgb8_cpu_reference(
        expected,
        {.target_dimensions = expected.dimensions,
         .output_origin_x = origin_x,
         .output_origin_y = origin_y}
    );
    const auto difference = fixture::rgb8_difference(actual.output->rgb8, display.bytes);
    require(
        difference.maximum <= 2 && difference.p99 <= 1,
        "Display CPU/CUDA mismatch: max=" + std::to_string(difference.maximum)
            + " p99=" + std::to_string(difference.p99)
    );
}

void resident_render_contract() {
    const auto source = fixture::make_random_image(257, 129, true);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    require(
        preparation.session != nullptr,
        "A real CUDA device is required: " + preparation.diagnostic
    );
    auto& session = *preparation.session;
    auto original = core_nodes();
    std::array<std::size_t, 4> order{0, 1, 2, 3};
    do {
        std::array
            nodes{original[order[0]], original[order[1]], original[order[2]], original[order[3]]};
        const auto plan = image::compile_edit_execution_plan(nodes);
        const auto expected = image::execute_adjustment_nodes_with_backend(
            source,
            nodes,
            {},
            image::AdjustmentBackendMode::cpu
        );
        compare(
            session.render(nodes, plan, true, {.display_origin_x = 113, .display_origin_y = 59}),
            expected.pixels,
            113,
            59
        );
    } while (std::next_permutation(order.begin(), order.end()));
    const std::array layers{
        image::AdjustmentLayer{
            .layer_id = "first",
            .opacity = .35,
            .nodes = {original[0], original[1]}
        },
        image::AdjustmentLayer{
            .layer_id = "second",
            .opacity = .62,
            .nodes = {original[2], original[3]}
        },
    };
    compare(session.render_layers(layers, true), image::execute_adjustment_layers(source, layers));
    const auto plan = image::compile_edit_execution_plan(original);
    auto first =
        std::async(std::launch::async, [&] { return session.render(original, plan, true); });
    auto second =
        std::async(std::launch::async, [&] { return session.render(original, plan, true); });
    const auto expected = image::execute_adjustment_nodes_with_backend(
        source,
        original,
        {},
        image::AdjustmentBackendMode::cpu
    );
    compare(first.get(), expected.pixels);
    compare(second.get(), expected.pixels);
    const auto fast = session.render(original, plan, false);
    require(
        fast.output && !fast.output->analyzed_linear && !fast.output->rgb8.empty(),
        "Interactive output reads back only packed display pixels"
    );
    require(
        session.stats().source_upload_count == 1,
        "Edits must reuse the immutable source upload"
    );
    const auto replay = session.host_source_for_cpu_replay();
    require(
        replay.source && replay.source->samples == source.samples,
        "Fallback reads the exact padded source"
    );
    require(
        session.host_source_for_cpu_replay().source == replay.source,
        "Fallback source is cached"
    );
    std::stop_source cancellation;
    cancellation.request_stop();
    require(
        session.render(original, plan, false, cancellation.get_token()).status
            == Session::RenderStatus::cancelled,
        "Cancelled edits must not publish pixels"
    );
    auto masked = layers;
    masked[0].mask = image::LocalMask{};
    require(
        session.render_layers(masked, false).status == Session::RenderStatus::unavailable_or_failed,
        "Unsupported masks must decline the whole GPU transaction"
    );
    std::cout << "CUDA: 24 ordered edit permutations, opacity, concurrency, cancellation, and CPU "
                 "parity passed\n";
}

void color_and_policy_contract() {
    auto source = fixture::make_random_image(37, 23, false);
    source.reference = image::ImageReference::display_referred;
    const auto session = image::detail::prepare_warm_edit_gpu_session(source);
    require(session.session != nullptr, session.diagnostic);
    compare(session.session->render({}, image::compile_edit_execution_plan({}), true), source);
    source.working_space.primaries[0].x = .7;
    require(
        !image::detail::prepare_warm_edit_gpu_session(source).session,
        "A matching color-space label cannot hide different primaries"
    );
    source = fixture::make_random_image(37, 23, false);
    image::test_support::ScopedEnvironment cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
    require(
        !image::detail::prepare_warm_edit_gpu_session(source).session,
        "CPU override disables resident CUDA"
    );
}

void curve_lut_and_failure_contract() {
    const auto source=fixture::make_random_image(133,79,true);
    auto preparation=image::detail::prepare_warm_edit_gpu_session(source);
    require(preparation.session != nullptr,preparation.diagnostic);
    image::CubeLut3D lut{.title="CUDA parity",.size=2};
    for(int b=0;b<2;++b) for(int g=0;g<2;++g) for(int r=0;r<2;++r)
        lut.entries.push_back({.05F+.85F*r+.05F*g,.03F+.90F*g,.02F+.87F*b+.03F*r});
    image::PerceptualColorAdjustment perceptual;
    perceptual.vibrance=.23;
    perceptual.hue[0]=.12;
    perceptual.saturation[3]=-.08;
    std::array nodes{
        image::AdjustmentNode{.node_id="curve",.parameters=image::OklabLightnessToneCurve{
            .lightness={.points={{0,0},{.2,.14},{.65,.74},{1,1.03}}}}},
        image::AdjustmentNode{.node_id="lut",.parameters=image::CubeLutAdjustment{.lut=std::move(lut),.intensity=.43}},
        image::AdjustmentNode{.node_id="perceptual",.parameters=perceptual},
    };
    const auto plan=image::compile_edit_execution_plan(nodes);
    for(int i=0;i<4;++i) {
        const auto expected=image::execute_adjustment_nodes_with_backend(source,nodes,{},image::AdjustmentBackendMode::cpu);
        compare(preparation.session->render(nodes,plan,true),expected.pixels);
    }
    const auto before=preparation.session->stats();
    compare(preparation.session->render(nodes,plan,true),
            image::execute_adjustment_nodes_with_backend(source,nodes,{},image::AdjustmentBackendMode::cpu).pixels);
    const auto after=preparation.session->stats();
    require(before.lut_resource_upload_count==after.lut_resource_upload_count
            && before.curve_resource_upload_count==after.curve_resource_upload_count,
            "Unchanged LUT and curve resources must stay resident");
    auto invalid_space=source;
    invalid_space.working_space.white_point.x=.35;
    require(!image::detail::prepare_warm_edit_gpu_session(invalid_space).session,
            "CUDA must decline a non-D65 source without relabeling its pixels");
}

void latency_contract() {
    const auto source = fixture::make_random_image(1024, 768, false);
    auto preparation = image::detail::prepare_warm_edit_gpu_session(source);
    require(preparation.session != nullptr, preparation.diagnostic);
    auto nodes = core_nodes();
    const auto plan = image::compile_edit_execution_plan(nodes);
    std::vector<double> milliseconds;
    for (int i = 0; i < 24; ++i) {
        nodes[1].parameters = image::ExposureAdjustment{.stops = .01 * i};
        const auto start = std::chrono::steady_clock::now();
        const auto result = preparation.session->render(nodes, plan, false);
        require(result.output.has_value(), result.diagnostic);
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count()
        );
    }
    const auto cold = milliseconds.front();
    milliseconds.erase(milliseconds.begin());
    std::sort(milliseconds.begin(), milliseconds.end());
    std::cout << "CUDA 1024x768 host RGB8: first=" << cold << "ms, warm p50=" << milliseconds[11]
              << "ms, p95=" << milliseconds[21]
              << "ms; source uploads=" << preparation.session->stats().source_upload_count << '\n';
}
} // namespace

int main() {
    try {
        image::test_support::ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
        resident_render_contract();
        color_and_policy_contract();
        curve_lut_and_failure_contract();
        latency_contract();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CUDA contract failed: " << error.what() << '\n';
        return 1;
    }
}
