#include "../src/optics/scene_linear_region_optics.hpp"
#include "../src/proxy/full_edit_detail_gpu_cache.hpp"
#include "../src/proxy/full_edit_detail_metal_source.hpp"
#include "../src/raw/raw_frame_region_development.hpp"
#include "../src/raw/raw_frame_source_preparation.hpp"
#include "../src/raw/resident_raw_source.hpp"
#include "raw_pipeline_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/source_rendering.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using shadow::image::test_support::ScopedEnvironment;

struct PublishedResident final {
    std::unique_ptr<image::raw_pipeline_detail::ResidentRawSource> source;
    image::AssetMetadata metadata;
};

[[nodiscard]] image::RawFrame
detail_fixture(const std::uint32_t width = 160U, const std::uint32_t height = 120U) {
    image::RawFrame frame = synthetic_bayer_frame();
    frame.descriptor.provider_id = "synthetic-metal-detail-provider";
    frame.descriptor.provider_version = "v1";
    frame.descriptor.storage_dimensions = {width, height};
    frame.descriptor.active_dimensions = frame.descriptor.storage_dimensions;
    frame.descriptor.active_margins = {};
    frame.descriptor.bits_per_sample = 12U;
    frame.descriptor.white_levels = {4'095U, 4'095U, 4'095U, 4'095U};
    frame.descriptor.as_shot_neutral = {0.72, 1.0, 1.0, 0.61};
    frame.samples.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            const std::uint32_t colour_base = colour == image::RawCfaColor::red     ? 620U
                                              : colour == image::RawCfaColor::green ? 1'140U
                                                                                    : 410U;
            const std::uint32_t gradient = (x * 1'700U) / std::max<std::uint32_t>(width - 1U, 1U)
                                           + (y * 900U) / std::max<std::uint32_t>(height - 1U, 1U);
            frame.samples[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint16_t>(std::min<std::uint32_t>(colour_base + gradient, 4'095U));
        }
    }
    return frame;
}

[[nodiscard]] image::RawDevelopmentPlan detail_plan() {
    image::RawDevelopmentPlan plan = image::default_raw_development_plan();
    plan.quality = image::RawDevelopmentQuality::high;
    plan.noise_reduction = image::RawNoiseReductionIntent::disabled;
    return plan;
}

[[nodiscard]] PublishedResident publish_resident(image::RawFrame frame) {
    SyntheticRawSession session(std::move(frame));
    const image::AssetMetadata metadata = session.metadata();
    const image::CameraProfileCatalog catalog{
        .identity = "shadow-camera-profile-catalog-v1:metal-detail-contract",
    };
    auto prepared = image::raw_pipeline_detail::prepare_raw_frame_source(
        session,
        detail_plan(),
        std::nullopt,
        catalog
    );
    auto optics = prepared.prepare_region_optics(nullptr, image::default_optics_settings());
    auto attempt = image::raw_pipeline_detail::try_prepare_metal_resident_raw_source(
        std::move(prepared),
        std::move(optics)
    );
    return PublishedResident{
        .source = std::move(attempt.source),
        .metadata = metadata,
    };
}

[[nodiscard]] std::array<image::AdjustmentNode, 2U> adjustment_nodes() {
    return {
        image::AdjustmentNode{
            .node_id = "metal-detail-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.27},
        },
        image::AdjustmentNode{
            .node_id = "metal-detail-contrast",
            .parameters = image::ContrastAdjustment{.factor = 1.08},
        },
    };
}

void native_source_adoption_has_no_intermediate_host_round_trip() {
    PublishedResident resident = publish_resident(detail_fixture());
    expect(resident.source != nullptr, "native detail contract publishes a resident RAW source");
    if (resident.source == nullptr) {
        return;
    }
    const image::SourceRenderingReceipt source_rendering =
        image::resolve_source_rendering(resident.metadata, resident.source->raw_pipeline_receipt());
    const image::GeometryPixelRect working_rect{17U, 13U, 96U, 72U};
    auto prepared = image::detail::prepare_full_edit_detail_metal_source(
        *resident.source,
        source_rendering,
        working_rect,
        0U
    );
    expect(prepared.published(), "RAW, optics, source rendering, and warm adoption publish once");
    if (!prepared.published()) {
        return;
    }
    const auto telemetry = prepared.telemetry;
    expect(
        telemetry.raw.source_upload_count == 1U && telemetry.raw.region_dispatch_count == 1U
            && telemetry.raw.region_readback_count == 0U
            && telemetry.raw.full_frame_readback_count == 0U
            && telemetry.optics.source_reupload_count == 0U
            && telemetry.optics.source_fp32_readback_count == 0U
            && telemetry.optics.debug_readback_count == 0U
            && telemetry.source_rendering_dispatch_count == 1U
            && telemetry.warm_source_adoption_count == 1U
            && telemetry.warm.source_upload_count == 0U
            && telemetry.combined_resident_bytes <= telemetry.resident_allowance_bytes,
        "the default native path keeps CFA and fp32 intermediates on one Metal device"
    );

    const auto nodes = adjustment_nodes();
    const image::EditExecutionPlan plan = image::compile_edit_execution_plan(nodes, 1.0, 1.0);
    const image::detail::WarmEditGpuRenderContext context{
        .adjustment =
            image::AdjustmentExecutionContext{
                .origin_x = working_rect.x,
                .origin_y = working_rect.y,
                .full_dimensions = resident.source->dimensions(),
            },
        .display_origin_x = working_rect.x,
        .display_origin_y = working_rect.y,
    };
    const auto first = prepared.session->render(nodes, plan, false, context);
    const auto repeated = prepared.session->render(nodes, plan, false, context);
    expect(
        first.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && repeated.status == image::detail::WarmEditGpuSession::RenderStatus::completed
            && first.output.has_value() && repeated.output.has_value()
            && first.output->rgb8 == repeated.output->rgb8
            && first.output->rgb8.size()
                   == static_cast<std::size_t>(working_rect.width) * working_rect.height * 3U
            && prepared.session->stats().source_upload_count == 0U
            && prepared.session->stats().completed_render_count == 2U,
        "one adopted warm session renders deterministic repeated tiles without a source upload"
    );
}

void resident_viewport_cache_reuses_the_adopted_session() {
    PublishedResident resident = publish_resident(detail_fixture());
    expect(resident.source != nullptr, "resident cache contract publishes a RAW source");
    if (resident.source == nullptr) {
        return;
    }
    const image::SourceRenderingReceipt source_rendering =
        image::resolve_source_rendering(resident.metadata, resident.source->raw_pipeline_receipt());
    image::detail::FullEditDetailGpuCache cache;
    const auto nodes = adjustment_nodes();
    const image::DetailTileRect tile{31U, 29U, 64U, 48U};
    const auto first = cache.render_resident(
        *resident.source,
        source_rendering,
        nodes,
        tile,
        tile,
        resident.source->dimensions()
    );
    const auto repeated = cache.render_resident(
        *resident.source,
        source_rendering,
        nodes,
        tile,
        tile,
        resident.source->dimensions()
    );
    const auto raw = resident.source->metal_source().telemetry();
    expect(
        first.bytes.has_value() && repeated.bytes.has_value() && first.bytes == repeated.bytes
            && !first.source_cache_hit && repeated.source_cache_hit
            && raw.region_dispatch_count == 1U && raw.region_readback_count == 0U,
        "the same viewport reuses one native source-rendered warm session"
    );
}

void resident_viewport_cache_rejects_a_terminally_invalidated_source() {
    PublishedResident resident = publish_resident(detail_fixture());
    expect(resident.source != nullptr, "terminal cache contract publishes a RAW source");
    if (resident.source == nullptr) {
        return;
    }
    const image::SourceRenderingReceipt source_rendering =
        image::resolve_source_rendering(resident.metadata, resident.source->raw_pipeline_receipt());
    image::detail::FullEditDetailGpuCache cache;
    const auto nodes = adjustment_nodes();
    const image::DetailTileRect tile{31U, 29U, 64U, 48U};
    const auto& raw_source = resident.source->metal_source();
    const auto first = cache.render_resident(
        *resident.source,
        source_rendering,
        nodes,
        tile,
        tile,
        resident.source->dimensions()
    );
    resident.source->invalidate_device_path();
    const auto terminal = cache.render_resident(
        *resident.source,
        source_rendering,
        nodes,
        tile,
        tile,
        resident.source->dimensions()
    );
    const auto telemetry = raw_source.telemetry();
    expect(
        first.bytes.has_value() && !terminal.bytes.has_value() && !terminal.source_cache_hit
            && terminal.diagnostic.find("terminally invalidated") != std::string::npos
            && telemetry.invalidated && telemetry.invalidation_count == 1U
            && telemetry.region_dispatch_count == 1U,
        "a cached warm viewport cannot bypass terminal invalidation of its RAW owner"
    );
}

void completed_source_rendering_is_preserved_when_warm_adoption_fails() {
    PublishedResident resident = publish_resident(detail_fixture());
    expect(resident.source != nullptr, "warm failure contract publishes a RAW source");
    if (resident.source == nullptr) {
        return;
    }
    const image::SourceRenderingReceipt source_rendering =
        image::resolve_source_rendering(resident.metadata, resident.source->raw_pipeline_receipt());
    const image::GeometryPixelRect working_rect{17U, 13U, 96U, 72U};
    const auto& raw_source = resident.source->metal_source();
    auto baseline = image::detail::prepare_full_edit_detail_metal_source(
        *resident.source,
        source_rendering,
        working_rect,
        0U
    );
    expect(baseline.published(), "warm failure contract first measures one real adoption");
    if (!baseline.published()) {
        return;
    }
    const std::uint64_t allowance = baseline.telemetry.resident_allowance_bytes;
    const std::uint64_t warm_bytes = baseline.telemetry.warm.resident_bytes;
    baseline.session.reset();
    const auto before = raw_source.telemetry();
    const std::uint64_t raw_bytes = raw_source.retained_bytes();
    if (allowance < raw_bytes || allowance - raw_bytes < warm_bytes) {
        expect(false, "the measured resident budget must contain its successful transaction");
        return;
    }
    const std::uint64_t output_bytes =
        static_cast<std::uint64_t>(working_rect.width) * working_rect.height * 3U * sizeof(float);
    const std::uint64_t preflight_bytes =
        output_bytes + baseline.telemetry.optics.coordinate_upload_bytes
        + baseline.telemetry.optics.profile_gain_upload_bytes
        + std::max<std::uint64_t>(baseline.telemetry.source_rendering_curve_upload_bytes, 1'024U);
    if (warm_bytes <= preflight_bytes + 1U) {
        expect(
            false,
            "the measured warm working set must leave room for a pre-adoption budget failure"
        );
        return;
    }
    const std::uint64_t other_cached_resident_bytes = allowance - raw_bytes - warm_bytes + 1U;
    auto prepared = image::detail::prepare_full_edit_detail_metal_source(
        *resident.source,
        source_rendering,
        working_rect,
        other_cached_resident_bytes
    );
    const auto after = raw_source.telemetry();
    auto terminal = image::detail::prepare_full_edit_detail_metal_source(
        *resident.source,
        source_rendering,
        working_rect,
        0U
    );
    const auto after_terminal_retry = raw_source.telemetry();
    expect(
        !prepared.published() && !resident.source->device_path_valid()
            && prepared.telemetry.raw.region_dispatch_count == before.region_dispatch_count + 1U
            && prepared.telemetry.raw.region_readback_count == 0U
            && prepared.telemetry.raw.full_frame_readback_count == 0U
            && prepared.telemetry.optics.optics_dispatch_count == 1U
            && prepared.telemetry.optics.source_reupload_count == 0U
            && prepared.telemetry.optics.source_fp32_readback_count == 0U
            && prepared.telemetry.source_rendering_dispatch_count == 1U
            && prepared.telemetry.warm_source_adoption_count == 0U
            && prepared.telemetry.warm.source_upload_count == 0U
            && prepared.diagnostic
                   == "warm-preview resident buffers exceed the combined Metal budget"
            && after.invalidated && after.invalidation_count == 1U && !terminal.published()
            && terminal.diagnostic.find("terminally invalidated") != std::string::npos
            && after_terminal_retry.region_dispatch_count == after.region_dispatch_count,
        "failure telemetry preserves every completed GPU stage before warm adoption"
    );
}

void public_cpu_and_metal_tiles_match_with_display_precision() {
    const image::RawFrame fixture = detail_fixture();
    const auto nodes = adjustment_nodes();
    const image::DetailTileRect tile{23U, 19U, 96U, 72U};
    const auto render_backend = [&](const std::string_view backend) {
        const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", backend);
        SyntheticRawSession session(fixture);
        auto detail = image::prepare_full_edit_detail(session, detail_plan());
        return detail.render_rgb8(nodes, tile);
    };
    const image::RenderedDetailTile cpu = render_backend("cpu");
    const image::RenderedDetailTile metal = render_backend("metal");
    std::uint32_t maximum_difference = 0U;
    if (cpu.bytes.size() == metal.bytes.size()) {
        for (std::size_t index = 0U; index < cpu.bytes.size(); ++index) {
            const auto left = static_cast<std::int32_t>(cpu.bytes[index]);
            const auto right = static_cast<std::int32_t>(metal.bytes[index]);
            maximum_difference = std::max<std::uint32_t>(
                maximum_difference,
                static_cast<std::uint32_t>(std::abs(left - right))
            );
        }
    }
    expect(
        cpu.execution.backend == image::DetailTileRenderBackend::cpu
            && metal.execution.backend == image::DetailTileRenderBackend::metal
            && !metal.execution.fell_back && cpu.bytes.size() == metal.bytes.size()
            && maximum_difference <= 1U,
        "the public native RAW tile matches the CPU oracle within one display code value"
    );
}

void public_automatic_route_reuses_one_resident_viewport() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "auto");
    SyntheticRawSession session(detail_fixture());
    auto detail = image::prepare_full_edit_detail(session, detail_plan());
    const auto nodes = adjustment_nodes();
    const image::DetailTileRect tile{23U, 19U, 96U, 72U};
    const auto first = detail.render_rgb8(nodes, tile);
    const auto repeated = detail.render_rgb8(nodes, tile);
    const std::uint64_t complete_fp32_bytes =
        static_cast<std::uint64_t>(detail.dimensions().pixel_count()) * 3U * sizeof(float);
    expect(
        session.raw_frame_count() == 1U && session.processed_count() == 0U
            && detail.retained_bytes() < complete_fp32_bytes
            && first.execution.backend == image::DetailTileRenderBackend::metal
            && !first.execution.source_cache_hit && repeated.execution.source_cache_hit
            && first.bytes == repeated.bytes,
        "automatic full-detail routing retains one CFA owner and reuses one native viewport"
    );
}

void automatic_prepublication_failure_materializes_the_intact_owner() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "auto");
    SyntheticRawSession session(detail_fixture());
    image::FullEditDetailSession detail = [&] {
        const ScopedEnvironment forced_failure("SHADOW_TEST_METAL_RESIDENT_FAIL_PREPARE", "1");
        return image::prepare_full_edit_detail(session, detail_plan());
    }();
    const std::uint64_t complete_fp32_bytes =
        static_cast<std::uint64_t>(detail.dimensions().pixel_count()) * 3U * sizeof(float);
    const auto rendered =
        detail.render_rgb8(adjustment_nodes(), image::DetailTileRect{23U, 19U, 96U, 72U});
    expect(
        session.raw_frame_count() == 1U && session.processed_count() == 0U
            && detail.retained_bytes() >= complete_fp32_bytes
            && detail.raw_pipeline_receipt().path == image::RawPipelinePath::shadow_raw_frame
            && detail.raw_development_receipt().recorded()
            && rendered.execution.backend == image::DetailTileRenderBackend::metal,
        "automatic failure before publication materializes the same decoded owner and provenance"
    );
}

void published_full_detail_failure_is_terminal_without_cpu_replay() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "metal");
    SyntheticRawSession session(detail_fixture());
    auto detail = image::prepare_full_edit_detail(session, detail_plan());
    const auto nodes = adjustment_nodes();
    const image::DetailTileRect tile{23U, 19U, 96U, 72U};
    {
        const ScopedEnvironment forced_failure(
            "SHADOW_TEST_FULL_EDIT_DETAIL_METAL_SOURCE_FAIL",
            "1"
        );
        try {
            static_cast<void>(detail.render_rgb8(nodes, tile));
            expect(false, "a published full-detail device failure must not replay on CPU");
        } catch (const image::EditError& error) {
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "a published full-detail device failure retains a typed backend error"
            );
        }
    }
    try {
        static_cast<void>(detail.render_rgb8(nodes, tile));
        expect(false, "an invalidated resident full-detail source must remain terminal");
    } catch (const image::EditError& error) {
        expect(
            error.code() == image::EditErrorCode::backend_failure && session.raw_frame_count() == 1U
                && session.processed_count() == 0U,
            "terminal invalidation never decodes again or substitutes provider/CPU pixels"
        );
    }
}

[[nodiscard]] bool benchmark_enabled() noexcept {
    const char* value = std::getenv("SHADOW_BENCH_FULL_EDIT_DETAIL_METAL_RAW");
    return value != nullptr && std::string_view(value) == "1";
}

[[nodiscard]] double percentile(std::vector<double> samples, const double ratio) {
    std::ranges::sort(samples);
    const std::size_t index = std::min(
        samples.size() - 1U,
        static_cast<std::size_t>(std::ceil(ratio * static_cast<double>(samples.size()))) - 1U
    );
    return samples[index];
}

struct BenchmarkSamples final {
    std::vector<double> prepare_ms;
    std::vector<double> first_tile_ms;
    std::vector<double> repeated_tile_ms;
    std::uint64_t checksum = 0U;
};

[[nodiscard]] std::uint64_t
consume_checksum(const std::span<const std::uint8_t> bytes, std::uint64_t checksum) noexcept {
    for (const std::uint8_t value : bytes) {
        checksum ^= value;
        checksum *= 1'099'511'628'211ULL;
    }
    return checksum;
}

[[nodiscard]] BenchmarkSamples
benchmark_backend(const image::RawFrame& fixture, const std::string_view backend) {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", backend);
    const ScopedEnvironment raw_pipeline("SHADOW_RAW_PIPELINE", "raw-frame");
    constexpr std::size_t iterations = 5U;
    BenchmarkSamples samples;
    samples.prepare_ms.reserve(iterations);
    samples.first_tile_ms.reserve(iterations);
    samples.repeated_tile_ms.reserve(iterations);
    samples.checksum = 1'469'598'103'934'665'603ULL;
    const auto nodes = adjustment_nodes();
    const image::DetailTileRect tile{1'536U, 1'024U, 1'024U, 1'024U};
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        SyntheticRawSession session(fixture);
        const auto prepare_begin = std::chrono::steady_clock::now();
        auto detail = image::prepare_full_edit_detail(session, detail_plan());
        const auto prepare_end = std::chrono::steady_clock::now();
        const auto first = detail.render_rgb8(nodes, tile);
        const auto first_end = std::chrono::steady_clock::now();
        const auto repeated = detail.render_rgb8(nodes, tile);
        const auto repeated_end = std::chrono::steady_clock::now();
        samples.prepare_ms.push_back(
            std::chrono::duration<double, std::milli>(prepare_end - prepare_begin).count()
        );
        samples.first_tile_ms.push_back(
            std::chrono::duration<double, std::milli>(first_end - prepare_end).count()
        );
        samples.repeated_tile_ms.push_back(
            std::chrono::duration<double, std::milli>(repeated_end - first_end).count()
        );
        samples.checksum = consume_checksum(first.bytes, samples.checksum);
        samples.checksum = consume_checksum(repeated.bytes, samples.checksum);
    }
    return samples;
}

void report_opt_in_benchmark() {
    if (!benchmark_enabled()) {
        return;
    }
    const image::RawFrame fixture = detail_fixture(4'096U, 3'072U);
    const BenchmarkSamples cpu = benchmark_backend(fixture, "cpu");
    const BenchmarkSamples metal = benchmark_backend(fixture, "metal");
    const auto report = [](const std::string_view backend, const BenchmarkSamples& samples) {
        std::cout << std::fixed << std::setprecision(3)
                  << "full_edit_detail_raw_benchmark backend=" << backend
                  << " prepare_median_ms=" << percentile(samples.prepare_ms, 0.5)
                  << " prepare_p90_ms=" << percentile(samples.prepare_ms, 0.9)
                  << " first_tile_median_ms=" << percentile(samples.first_tile_ms, 0.5)
                  << " first_tile_p90_ms=" << percentile(samples.first_tile_ms, 0.9)
                  << " repeated_tile_median_ms=" << percentile(samples.repeated_tile_ms, 0.5)
                  << " repeated_tile_p90_ms=" << percentile(samples.repeated_tile_ms, 0.9)
                  << " checksum=" << samples.checksum << '\n';
    };
    report("cpu", cpu);
    report("metal", metal);
}

} // namespace

int main() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "auto");
    const ScopedEnvironment raw_pipeline("SHADOW_RAW_PIPELINE", "raw-frame");
    if (!image::raw_pipeline_detail::metal_resident_raw_source_available()
        || !image::detail::full_edit_detail_metal_source_available()) {
        std::cout << "full-detail resident Metal RAW path is unavailable\n";
        if (std::getenv("SHADOW_TEST_REQUIRE_METAL") != nullptr) {
            expect(false, "Metal was required for full-detail resident RAW validation");
        }
        return failures == 0 ? 0 : 1;
    }
    native_source_adoption_has_no_intermediate_host_round_trip();
    resident_viewport_cache_reuses_the_adopted_session();
    resident_viewport_cache_rejects_a_terminally_invalidated_source();
    completed_source_rendering_is_preserved_when_warm_adoption_fails();
    public_cpu_and_metal_tiles_match_with_display_precision();
    public_automatic_route_reuses_one_resident_viewport();
    automatic_prepublication_failure_materializes_the_intact_owner();
    published_full_detail_failure_is_terminal_without_cpu_replay();
    report_opt_in_benchmark();
    return failures == 0 ? 0 : 1;
}
