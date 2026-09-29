#include "../src/proxy/warm_edit_gpu.hpp"
#include "edit_contract_test_support.hpp"
#include "working_color_math.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/paint.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/warm_edit_preview.hpp>
namespace image = shadow::image;
using namespace shadow::image::test;
namespace {
image::FloatRgbImage input(std::uint32_t w = 192, std::uint32_t h = 128) {
    auto result = rgb_raster(w, h, std::vector<float>(static_cast<std::size_t>(w) * h * 3, 0.2f));
    result.working_space = linear_srgb();
    return result;
}
image::PaintLayerAdjustment paint() {
    return {
        .coordinate_width = 6000,
        .coordinate_height = 4000,
        .blend = 0,
        .opacity = 0.75,
        .strokes = {
            {.points = {{0.2, 0.3, 0.5}, {0.65, 0.55, 1}},
             .radius = 0.035,
             .hardness = 0.3,
             .opacity = 0.8,
             .flow = 0.15,
             .color = {0.8, 0.2, 0.1}}
        }
    };
}
image::FloatRgbImage render(
    const image::FloatRgbImage& source,
    const image::PaintLayerAdjustment& layer,
    image::AdjustmentExecutionContext context = {}
) {
    return image::execute_adjustment_nodes(
        source,
        std::array{image::AdjustmentNode{.node_id = "paint", .parameters = layer}},
        context
    );
}
void immutable_input_and_eraser() {
    auto source = input();
    auto layer = paint();
    layer.opacity = 1;
    layer.strokes[0].opacity = 1;
    layer.strokes[0].flow = 1;
    auto painted = render(source, layer);
    expect(painted.samples != source.samples, "paint changes covered pixels");
    expect(source.samples == input().samples, "painting preserves original input");
    expect(painted.samples.front() == source.samples.front(), "outside coverage stays exact");
    auto erase = layer.strokes[0];
    erase.erase = true;
    erase.radius = 0.1;
    erase.hardness = 1;
    for (auto& point : erase.points)
        point.pressure = 1;
    layer.strokes.push_back(erase);
    auto cleared = render(source, layer);
    for (std::size_t i = 0; i < source.samples.size(); ++i)
        expect_close(
            cleared.samples[i],
            source.samples[i],
            "eraser reveals original without white paint"
        );
}
void tile_and_pressure_contract() {
    const auto source = input();
    auto layer = paint();
    const auto full = render(source, layer);
    auto tile = input(93, 51);
    const auto result =
        render(tile, layer, {.origin_x = 41, .origin_y = 32, .full_dimensions = source.dimensions});
    for (std::uint32_t y = 0; y < 51; ++y)
        for (std::uint32_t x = 0; x < 93; ++x)
            for (std::size_t c = 0; c < 3; ++c)
                expect_close(
                    result.samples[(static_cast<std::size_t>(y) * 93 + x) * 3 + c],
                    full.samples[(static_cast<std::size_t>(y + 32) * 192 + x + 41) * 3 + c],
                    "paint has no full-detail tile seam"
                );
    for (auto& p : layer.strokes[0].points)
        p.pressure = 0;
    expect(render(source, layer).samples == source.samples, "zero pressure lays no ink");
}
void brush_tip_and_dynamics_contract() {
    auto layer = paint();
    auto& brush = layer.strokes[0];
    brush.points = {{0.5, 0.5, 1}};
    brush.radius = 0.2;
    brush.hardness = 1;
    brush.flow = 1;
    brush.roundness = 0.2;
    const auto source = input(160, 160);
    const auto horizontal = render(source, layer);
    const auto at = [](const auto& im, int x, int y) {
        return im.samples[(static_cast<std::size_t>(y) * 160 + static_cast<std::size_t>(x)) * 3];
    };
    expect(
        at(horizontal, 95, 80) != 0.2f && at(horizontal, 80, 95) == 0.2f,
        "ellipse has the authored axes"
    );
    brush.angle_degrees = 90;
    const auto vertical = render(source, layer);
    expect(
        at(vertical, 95, 80) == 0.2f && at(vertical, 80, 95) != 0.2f,
        "angle rotates the ellipse"
    );
    brush.roundness = 1;
    brush.angle_degrees = 0;
    brush.pressure_size = true;
    brush.pressure_flow = false;
    brush.points[0].pressure = 0.1;
    const auto small = render(source, layer);
    brush.points[0].pressure = 1;
    expect(
        at(small, 95, 80) == 0.2f && at(render(source, layer), 95, 80) != 0.2f,
        "pressure changes size separately from flow"
    );
    const auto full_pressure = render(source, layer);
    brush.points = {{0.5, 0.5, 0.1}, {0.5, 0.5, 1.0}};
    expect(
        render(source, layer).samples == full_pressure.samples,
        "stationary pressure increase updates the contact dab"
    );
    brush.points.resize(1);
    brush.pressure_size = false;
    brush.points[0].pressure = 0.1;
    expect(
        render(source, layer).samples != source.samples,
        "disabled pressure flow retains ink at low pressure"
    );
    brush.points = {{0.2, 0.5, 1}, {0.8, 0.5, 1}};
    brush.radius = 0.04;
    brush.flow = 0.1;
    brush.spacing = 0.05;
    const auto dense = render(source, layer);
    brush.spacing = 0.8;
    const auto sparse = render(source, layer);
    expect(dense.samples != sparse.samples, "spacing changes overlap without changing opacity");
    for (std::uint8_t texture = 1; texture <= 2; ++texture) {
        brush.texture = texture;
        brush.texture_strength = 0.8;
        brush.spacing = 0.125;
        brush.roundness = 0.4;
        brush.angle_degrees = 32;
        brush.pressure_size = true;
        brush.points = {{0.2, 0.3, 0.2}, {0.7, 0.6, 0.9}};
        const auto full = render(source, layer);
        expect(full.samples == render(source, layer).samples, "texture replay is deterministic");
        const auto first = brush.points.front(), last = brush.points.back();
        brush.points.insert(
            brush.points.begin() + 1,
            {(first.x + last.x) / 2, (first.y + last.y) / 2, (first.pressure + last.pressure) / 2}
        );
        const auto segmented = render(source, layer);
        for (std::size_t i = 0; i < full.samples.size(); ++i)
            expect_close_double(
                segmented.samples[i],
                full.samples[i],
                1e-6,
                "adaptive pressure spacing is independent of event density"
            );
        auto tile = render(
            input(71, 63),
            layer,
            {.origin_x = 41, .origin_y = 37, .full_dimensions = source.dimensions}
        );
        for (std::uint32_t y = 0; y < 63; ++y)
            for (std::uint32_t x = 0; x < 71; ++x)
                for (std::size_t c = 0; c < 3; ++c)
                    expect_close(
                        tile.samples[(static_cast<std::size_t>(y) * 71 + x) * 3 + c],
                        full.samples[(static_cast<std::size_t>(y + 37) * 160 + x + 41) * 3 + c],
                        "textured ellipse has no tile seams"
                    );
        if (image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
            const auto gpu = image::execute_adjustment_nodes_with_backend(
                source,
                std::array{image::AdjustmentNode{.node_id = "paint", .parameters = layer}},
                {},
                image::AdjustmentBackendMode::metal
            );
            expect(!gpu.fell_back, "dynamic texture uses real Metal");
            for (std::size_t i = 0; i < full.samples.size(); ++i)
                expect_close_double(
                    full.samples[i],
                    gpu.pixels.samples[i],
                    2e-5,
                    "dynamic brush CPU/Metal parity"
                );
        }
    }
}
void perceptual_color_and_gpu_parity() {
    const auto source = input();
    auto layer = paint();
    const auto transform = image::detail::prepare_working_space_transform(source.working_space);
    const double original = image::detail::working_rgb_to_oklab(transform, {0.2, 0.2, 0.2})[0];
    for (std::uint8_t mode = 0; mode < 3; ++mode) {
        layer.blend = mode;
        auto cpu = render(source, layer);
        if (mode == 1)
            for (std::size_t i = 0; i < cpu.samples.size(); i += 3)
                expect_close_double(
                    image::detail::working_rgb_to_oklab(
                        transform,
                        {cpu.samples[i], cpu.samples[i + 1], cpu.samples[i + 2]}
                    )[0],
                    original,
                    1e-6,
                    "Color paint preserves Oklab lightness"
                );
        std::cout << "paint blend " << unsigned(mode) << " Metal available="
                  << image::adjustment_backend_available(image::AdjustmentBackend::metal) << "\n";
        if (image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
            auto gpu = image::execute_adjustment_nodes_with_backend(
                source,
                std::array{image::AdjustmentNode{.node_id = "paint", .parameters = layer}},
                {},
                image::AdjustmentBackendMode::metal
            );
            expect(
                gpu.backend == image::AdjustmentBackend::metal && !gpu.fell_back,
                "paint executes on real Metal"
            );
            for (std::size_t i = 0; i < cpu.samples.size(); ++i)
                expect_close_double(
                    gpu.pixels.samples[i],
                    cpu.samples[i],
                    2e-5,
                    "CPU/Metal paint parity"
                );
        }
    }
}
void resident_editing_and_sampling_contract() {
    const auto source = input(1537, 1025);
    auto layer = paint();
    layer.strokes[0].radius = 0.012;
    layer.strokes[0].roundness = 0.35;
    layer.strokes[0].angle_degrees = 37;
    layer.strokes[0].texture = 2;
    layer.strokes[0].texture_strength = 0.7;
    layer.strokes[0].pressure_size = true;
    layer.strokes[0].points.front().pressure = 0.2;
    auto prepared = image::detail::prepare_warm_edit_gpu_session(source);
    if (!prepared.session) {
        expect(
            std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
            "resident Metal paint required"
        );
        return;
    }
    const auto initial = prepared.session->stats();
    for (std::uint8_t blend = 0; blend < 3; ++blend) {
        layer.blend = blend;
        const std::array nodes{image::AdjustmentNode{.node_id = "paint", .parameters = layer}};
        const auto begin = std::chrono::steady_clock::now();
        const auto result =
            prepared.session->render(nodes, image::compile_edit_execution_plan(nodes), true);
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        std::cout << "warm paint blend=" << unsigned(blend) << " milliseconds=" << elapsed
                  << " diagnostic=" << result.diagnostic << "\n";
        expect(
            result.status == image::detail::WarmEditGpuSession::RenderStatus::completed
                && result.output && result.output->analyzed_linear,
            "paint remains resident on Metal"
        );
        if (result.output && result.output->analyzed_linear) {
            const auto expected = render(source, layer);
            const auto& actual = result.output->analyzed_linear->samples;
            for (std::size_t i = 0; i < actual.size(); ++i)
                if (std::abs(actual[i] - expected.samples[i]) > 3e-5) {
                    expect(false, "warm Metal paint matches tiled CPU oracle");
                    break;
                }
        }
    }
    const auto after = prepared.session->stats();
    expect(
        after.source_upload_count == initial.source_upload_count,
        "paint edits reuse original source upload"
    );
    expect(
        after.resource_cache_hit_count > initial.resource_cache_hit_count,
        "blend changes reuse paint GPU side resource"
    );
    image::PhotoLiquify liquify{
        .strokes = {image::PhotoLiquifyPushStroke{
            .points = {{0.3, 0.5, 1}, {0.5, 0.5, 1}},
            .radius = 0.2,
            .strength = 0.5,
            .hardness = 0.5
        }}
    };
    const auto warp = image::prepare_photo_liquify({6000, 4000}, liquify);
    const auto mapped = image::photo_liquify_source_point(warp, {0.5, 0.5, 1});
    expect(
        mapped.x < 0.5 && mapped.x > 0.2,
        "paint pointer maps through Liquify inverse displacement"
    );
    expect_close_double(mapped.y, 0.5, 1e-12, "horizontal Liquify preserves pointer y");
}
void neutral_soft_light_and_path_sampling() {
    auto layer = paint();
    layer.blend = 2;
    layer.strokes[0].color = {0.5, 0.5, 0.5};
    const auto source = input();
    const auto gray = render(source, layer);
    for (std::size_t i = 0; i < gray.samples.size(); ++i)
        expect_close_double(
            gray.samples[i],
            source.samples[i],
            1e-6,
            "50 percent gray is neutral Soft Light"
        );
    layer = paint();
    const auto sparse = render(source, layer);
    const auto a = layer.strokes[0].points.front(), b = layer.strokes[0].points.back();
    layer.strokes[0].points.insert(
        layer.strokes[0].points.begin() + 1,
        {(a.x + b.x) / 2, (a.y + b.y) / 2, (a.pressure + b.pressure) / 2}
    );
    const auto dense = render(source, layer);
    for (std::size_t i = 0; i < dense.samples.size(); ++i)
        expect_close_double(
            dense.samples[i],
            sparse.samples[i],
            1e-6,
            "pointer event density does not alter flow"
        );
}

} // namespace
int main() {
    immutable_input_and_eraser();
    brush_tip_and_dynamics_contract();
    tile_and_pressure_contract();
    perceptual_color_and_gpu_parity();
    neutral_soft_light_and_path_sampling();
    resident_editing_and_sampling_contract();
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
