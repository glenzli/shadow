#include <shadow/image/fused_raw_development.hpp>

#include "bayer_sampling.hpp"
#include "metal_raw_development.hpp"
#include "../concurrency/row_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image {

namespace {

using CameraRgbSample = detail::CameraRgbSample;
inline constexpr std::string_view raw_acceleration_environment =
    "SHADOW_IMAGE_ACCELERATION";

void validate_request(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawDevelopmentQuality quality,
    const RawHighlightRecoveryIntent highlight_recovery
) {
    detail::validate_bayer_frame(frame, "fused Bayer development");
    if (!transform.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "fused Bayer development requires a finite non-zero camera transform"
        );
    }
    const auto orientation = frame.descriptor.orientation;
    if (orientation != 0 && orientation != 3 && orientation != 5 && orientation != 6) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "fused Bayer development does not support this source orientation"
        );
    }
    if (preview_max_edge.has_value() && *preview_max_edge == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "fused Bayer preview max edge must be non-zero"
        );
    }
    if (
        quality != RawDevelopmentQuality::fast
        && quality != RawDevelopmentQuality::balanced
        && quality != RawDevelopmentQuality::high
    ) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "fused Bayer development received an unknown RAW quality tier"
        );
    }
    if (
        highlight_recovery != RawHighlightRecoveryIntent::provider_default
        && highlight_recovery != RawHighlightRecoveryIntent::disabled
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "fused Bayer development does not implement the requested highlight recovery"
        );
    }
}

[[nodiscard]] Dimensions oriented_dimensions(
    const Dimensions dimensions,
    const std::int32_t orientation
) noexcept {
    return orientation == 5 || orientation == 6
        ? Dimensions{dimensions.height, dimensions.width}
        : dimensions;
}

// Maps one display-oriented output coordinate back to the un-oriented reconstruction raster.
// These are the same LibRaw orientation semantics used by the existing RawFrame developer.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> source_coordinate(
    const std::uint32_t output_x,
    const std::uint32_t output_y,
    const Dimensions source,
    const std::int32_t orientation
) noexcept {
    switch (orientation) {
    case 3:
        return {source.width - 1U - output_x, source.height - 1U - output_y};
    case 5:
        return {source.width - 1U - output_y, output_x};
    case 6:
        return {output_y, source.height - 1U - output_x};
    case 0:
    default:
        return {output_x, output_y};
    }
}

[[nodiscard]] double smoothstep(
    const double edge0,
    const double edge1,
    const double value
) noexcept {
    const double normalized = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

// CFA channels do not necessarily hit sensor white at the same time. Once multiple reconstructed
// neighbourhoods are clipped — or one is clipped while the other two are also near white — the
// colour ratio is no longer a measurement. Neutralize only that irrecoverable highlight before
// per-channel clipping so a camera matrix cannot turn it into a magenta or green false colour.
void neutralize_sensor_clipped_highlight(
    std::array<double, 3U>& scene_linear,
    const CameraRgbSample& camera
) noexcept {
    const double lowest = std::min({
        static_cast<double>(camera.sensor_clip_coverage[0]),
        static_cast<double>(camera.sensor_clip_coverage[1]),
        static_cast<double>(camera.sensor_clip_coverage[2]),
    });
    const double highest = std::max({
        static_cast<double>(camera.sensor_clip_coverage[0]),
        static_cast<double>(camera.sensor_clip_coverage[1]),
        static_cast<double>(camera.sensor_clip_coverage[2]),
    });
    const double second_highest = static_cast<double>(camera.sensor_clip_coverage[0])
        + static_cast<double>(camera.sensor_clip_coverage[1])
        + static_cast<double>(camera.sensor_clip_coverage[2]) - lowest - highest;
    const double camera_lowest = std::min({
        static_cast<double>(camera.values[0]),
        static_cast<double>(camera.values[1]),
        static_cast<double>(camera.values[2]),
    });
    const double camera_highest = std::max({
        static_cast<double>(camera.values[0]),
        static_cast<double>(camera.values[1]),
        static_cast<double>(camera.values[2]),
    });
    const double camera_second_highest = static_cast<double>(camera.values[0])
        + static_cast<double>(camera.values[1])
        + static_cast<double>(camera.values[2]) - camera_lowest - camera_highest;
    const double multi_channel_clip = smoothstep(0.15, 0.75, second_highest);
    const double single_channel_white = smoothstep(0.40, 0.90, highest)
        * smoothstep(0.84, 0.98, camera_second_highest);
    const double clipped_ratio = std::max(multi_channel_clip, single_channel_white);
    const double peak = std::max({scene_linear[0], scene_linear[1], scene_linear[2]});
    const double highlight_ratio = smoothstep(0.85, 1.05, peak);
    const double blend = clipped_ratio * highlight_ratio;
    if (blend == 0.0) {
        return;
    }
    const double neutral = std::max(0.0, peak);
    for (double& value : scene_linear) {
        value += (neutral - value) * blend;
    }
}

void write_transformed_pixel(
    const CameraRgbSample& camera,
    const RawFrameLinearTransform& transform,
    const bool neutralize_clipped_highlights,
    float* destination
) noexcept {
    std::array<double, 3U> scene_linear{};
    for (std::size_t output = 0U; output < 3U; ++output) {
        double value = 0.0;
        for (std::size_t input = 0U; input < 3U; ++input) {
            value += transform.camera_to_linear_srgb_d65[output * 3U + input]
                * static_cast<double>(camera.values[input]);
        }
        scene_linear[output] = value;
    }
    if (neutralize_clipped_highlights) {
        neutralize_sensor_clipped_highlight(scene_linear, camera);
    }
    for (std::size_t output = 0U; output < 3U; ++output) {
        destination[output] = static_cast<float>(scene_linear[output]);
    }
}

[[nodiscard]] SceneLinearRgbFrame allocate_output(const Dimensions dimensions) {
    const auto sample_count = static_cast<std::uint64_t>(dimensions.width)
        * dimensions.height * 3U;
    if (sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "fused Bayer development output exceeds the address space"
        );
    }
    SceneLinearRgbFrame output;
    output.dimensions = dimensions;
    output.row_stride_bytes =
        static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float);
    output.samples.resize(static_cast<std::size_t>(sample_count));
    return output;
}

[[nodiscard]] RawDemosaicReceipt make_demosaic_receipt(
    const RawFrame& frame,
    const RawDemosaicAlgorithm algorithm
) noexcept {
    return RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = frame.descriptor.schema_version,
        .algorithm = algorithm,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = false,
        .dng_opcodes_applied = false,
    };
}

[[nodiscard]] bool valid_demosaic_receipt(const RawDemosaicReceipt& receipt) noexcept {
    return receipt.schema_version == raw_demosaic_receipt_schema_version
        && receipt.source_raw_frame_schema_version == raw_frame_schema_version
        && receipt.black_subtraction_applied
        && receipt.white_level_normalization_applied
        && !receipt.white_balance_applied
        && !receipt.dng_opcodes_applied;
}

[[nodiscard]] FusedRawFrameDevelopment develop_on_cpu(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawDevelopmentQuality quality,
    const RawHighlightRecoveryIntent highlight_recovery
) {
    const auto& descriptor = frame.descriptor;
    const Dimensions reconstruction_dimensions = preview_max_edge.has_value()
        ? proxy_dimensions(descriptor.active_dimensions, *preview_max_edge)
        : descriptor.active_dimensions;
    const bool area_preview = reconstruction_dimensions != descriptor.active_dimensions;
    const auto area_sampling = area_preview
        ? std::optional<detail::BayerAreaSamplingGrid>(
            detail::make_bayer_area_sampling_grid(frame, reconstruction_dimensions)
        )
        : std::nullopt;
    const Dimensions output_dimensions = oriented_dimensions(
        reconstruction_dimensions,
        descriptor.orientation
    );
    SceneLinearRgbFrame output = allocate_output(output_dimensions);

    detail::parallel_for_rows(
        output_dimensions.height,
        area_preview ? 8U : 16U,
        [&frame,
         &transform,
         &output,
         reconstruction_dimensions,
         output_dimensions,
         area_preview,
         area_sampling,
         quality,
         highlight_recovery](
            const std::uint32_t first_row,
            const std::uint32_t last_row
        ) {
            for (std::uint32_t output_y = first_row; output_y < last_row; ++output_y) {
                for (std::uint32_t output_x = 0U;
                     output_x < output_dimensions.width;
                     ++output_x) {
                    const auto [source_x, source_y] = source_coordinate(
                        output_x,
                        output_y,
                        reconstruction_dimensions,
                        frame.descriptor.orientation
                    );
                    const CameraRgbSample camera = area_preview
                        ? detail::area_camera_rgb_sample_at(
                            frame,
                            *area_sampling,
                            source_x,
                            source_y
                        )
                        : quality == RawDevelopmentQuality::high
                            ? detail::edge_aware_camera_rgb_sample_at(
                                frame,
                                frame.descriptor.active_margins.left + source_x,
                                frame.descriptor.active_margins.top + source_y
                            )
                            : detail::bilinear_camera_rgb_sample_at(
                                frame,
                                frame.descriptor.active_margins.left + source_x,
                                frame.descriptor.active_margins.top + source_y
                            );
                    const auto output_index =
                        (static_cast<std::size_t>(output_y) * output_dimensions.width + output_x)
                        * 3U;
                    write_transformed_pixel(
                        camera,
                        transform,
                        highlight_recovery
                            == RawHighlightRecoveryIntent::provider_default,
                        output.samples.data() + output_index
                    );
                }
            }
        }
    );

    FusedRawFrameDevelopment result{
        .scene_linear = std::move(output),
        .demosaic_receipt = make_demosaic_receipt(
            frame,
            area_preview
                ? RawDemosaicAlgorithm::bayer_area_preview_v1
                : quality == RawDevelopmentQuality::high
                    ? RawDemosaicAlgorithm::bayer_edge_aware_v1
                    : RawDemosaicAlgorithm::bayer_bilinear_v1
        ),
        .backend = RawDevelopmentBackend::cpu,
        .highlight_recovery = highlight_recovery,
    };
    if (!result.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "fused CPU Bayer development produced an invalid scene-linear raster"
        );
    }
    return result;
}

} // namespace

std::string_view raw_development_backend_identity(
    const RawDevelopmentBackend backend
) noexcept {
    switch (backend) {
    case RawDevelopmentBackend::cpu:
        return "shadow-fused-raw-cpu-v1;demosaic=plan-selected;"
            "sensor-highlight-policy=explicit";
    case RawDevelopmentBackend::metal:
        return "shadow-fused-raw-metal-v1;math=f32-precise;"
            "demosaic=plan-selected;sensor-highlight-policy=explicit";
    }
    return "shadow-fused-raw-unknown";
}

std::string_view raw_highlight_treatment_identity(
    const RawHighlightRecoveryIntent intent
) noexcept {
    switch (intent) {
    case RawHighlightRecoveryIntent::provider_default:
        return "sensor-highlights=neutral-v1";
    case RawHighlightRecoveryIntent::disabled:
        return "sensor-highlights=disabled";
    case RawHighlightRecoveryIntent::conservative:
    case RawHighlightRecoveryIntent::aggressive:
        return "sensor-highlights=unsupported";
    }
    return "sensor-highlights=unknown";
}

bool raw_development_backend_available(const RawDevelopmentBackend backend) noexcept {
    switch (backend) {
    case RawDevelopmentBackend::cpu:
        return true;
    case RawDevelopmentBackend::metal:
        return detail::metal_raw_development_available();
    }
    return false;
}

RawDevelopmentBackendMode raw_development_backend_mode_from_environment() {
    const auto* configured = std::getenv(raw_acceleration_environment.data());
    if (configured == nullptr || *configured == '\0'
        || std::string_view(configured) == "auto") {
        return RawDevelopmentBackendMode::automatic;
    }
    if (std::string_view(configured) == "cpu") {
        return RawDevelopmentBackendMode::cpu;
    }
    if (std::string_view(configured) == "metal") {
        return RawDevelopmentBackendMode::metal;
    }
    throw DecodeError(
        DecodeErrorCode::invalid_request,
        0,
        "SHADOW_IMAGE_ACCELERATION must be auto, cpu, or metal"
    );
}

bool RawFrameLinearTransform::valid() const noexcept {
    bool non_zero = false;
    for (const double coefficient : camera_to_linear_srgb_d65) {
        if (!std::isfinite(coefficient)) {
            return false;
        }
        non_zero = non_zero || coefficient != 0.0;
    }
    return non_zero;
}

bool FusedRawFrameDevelopment::valid() const noexcept {
    const auto width = static_cast<std::uint64_t>(scene_linear.dimensions.width);
    const auto height = static_cast<std::uint64_t>(scene_linear.dimensions.height);
    const bool known_backend = backend == RawDevelopmentBackend::cpu
        || backend == RawDevelopmentBackend::metal;
    const bool known_highlight_treatment =
        highlight_recovery == RawHighlightRecoveryIntent::provider_default
        || highlight_recovery == RawHighlightRecoveryIntent::disabled;
    if (!known_backend || !known_highlight_treatment
        || width == 0U || height == 0U || !scene_linear.valid()
        || !valid_demosaic_receipt(demosaic_receipt)) {
        return false;
    }
    return true;
}

FusedRawFrameDevelopment develop_bayer_linear_srgb_f32_fused(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawHighlightRecoveryIntent highlight_recovery,
    const RawDevelopmentQuality quality
) {
    return develop_bayer_linear_srgb_f32_fused_with_backend(
        frame,
        transform,
        preview_max_edge,
        raw_development_backend_mode_from_environment(),
        highlight_recovery,
        quality
    );
}

FusedRawFrameDevelopment develop_bayer_linear_srgb_f32_fused_with_backend(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawDevelopmentBackendMode backend_mode,
    const RawHighlightRecoveryIntent highlight_recovery,
    const RawDevelopmentQuality quality
) {
    validate_request(frame, transform, preview_max_edge, quality, highlight_recovery);
    if (backend_mode != RawDevelopmentBackendMode::cpu) {
        auto attempt = detail::try_develop_bayer_linear_srgb_f32_metal(
            frame,
            transform,
            preview_max_edge,
            highlight_recovery,
            quality
        );
        if (attempt.development.has_value()) {
            return std::move(*attempt.development);
        }
        if (backend_mode == RawDevelopmentBackendMode::metal) {
            const std::string diagnostic = attempt.diagnostic.empty()
                ? "Metal RAW development is unavailable" : std::move(attempt.diagnostic);
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                diagnostic
            );
        }
    }
    auto result = develop_on_cpu(frame, transform, preview_max_edge, quality, highlight_recovery);
    if (!result.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "fused RAW backend selection produced an invalid scene-linear raster"
        );
    }
    return result;
}

} // namespace shadow::image
