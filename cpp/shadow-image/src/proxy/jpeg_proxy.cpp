#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/warm_edit_preview.hpp>
#include <shadow/image/working_rgb.hpp>

#include "developed_source_raster.hpp"
#include "display_rgb_math.hpp"
#include "jpeg_proxy_encoding.hpp"
#include "proxy_render_request_validation.hpp"
#include "warm_edit_gpu.hpp"
#include "../concurrency/row_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace shadow::image {

namespace {

void validate_warm_edit_max_edge(const std::uint32_t max_edge) {
    if (max_edge == 0U || max_edge > maximum_warm_edit_preview_edge) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "warm edit preview max edge must be in 1..=4096"
        );
    }
}

// Packed-provider RGB optics stays on its original u16 path. For an interactive preview, reduce
// the source before entering the provider and round-trip only the bounded proxy through that API.
// This avoids a 45 MP geometry remap just to display 1200 px, while full-detail sessions still
// run on the complete native reference. JPEG/HEIF remain excluded so they cannot be silently
// double-corrected.
[[nodiscard]] PixelBuffer working_to_linear_reference(const FloatRgbImage& source) {
    if (
        source.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || source.transfer_function != TransferFunction::linear
        || (source.reference != ImageReference::scene_referred
            && source.reference != ImageReference::display_referred)
        || source.dimensions.width == 0U || source.dimensions.height == 0U
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "preview optics conversion requires standardized linear interleaved RGB float pixels"
        );
    }
    const std::size_t expected_samples =
        proxy_detail::checked_interleaved_rgb_sample_count(source.dimensions);
    if (source.samples.size() != expected_samples) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "preview optics conversion found an invalid float RGB layout"
        );
    }
    PixelBuffer output;
    output.dimensions = source.dimensions;
    output.bits_per_channel = 16U;
    output.channels = 3U;
    output.row_stride_bytes = source.dimensions.width * 3U * sizeof(std::uint16_t);
    output.primaries = RgbPrimaries::srgb_rec709_d65;
    output.transfer_function = RgbTransferFunction::linear;
    output.reference = source.reference == ImageReference::scene_referred
        ? RgbBufferReference::processed_raw
        : RgbBufferReference::decoded_raster;
    output.samples.resize(expected_samples);
    for (std::size_t index = 0U; index < expected_samples; ++index) {
        output.samples[index] = static_cast<std::uint16_t>(std::clamp(
            std::llround(std::clamp(static_cast<double>(source.samples[index]), 0.0, 1.0)
                * 65'535.0),
            0LL,
            65'535LL
        ));
    }
    return output;
}

// Shadow-owned RawFrame development never crosses the packed provider-RGB boundary. This tiny
// adapter intentionally preserves every finite scene-linear float, including values above one
// and small negative gamut components, for providers that explicitly advertise float support.
[[nodiscard]] SceneLinearRgbFrame working_to_scene_linear_reference(
    const FloatRgbImage& source
) {
    if (
        source.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || source.transfer_function != TransferFunction::linear
        || source.reference != ImageReference::scene_referred
        || source.dimensions.width == 0U || source.dimensions.height == 0U
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "scene-linear preview optics requires standardized scene-referred RGB float pixels"
        );
    }
    const std::size_t expected_samples =
        proxy_detail::checked_interleaved_rgb_sample_count(source.dimensions);
    const std::size_t expected_stride =
        static_cast<std::size_t>(source.dimensions.width) * 3U * sizeof(float);
    if (
        source.row_stride_bytes != expected_stride || source.samples.size() != expected_samples
    ) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "scene-linear preview optics found an invalid float RGB layout"
        );
    }
    SceneLinearRgbFrame output{
        .dimensions = source.dimensions,
        .row_stride_bytes = expected_stride,
        .samples = source.samples,
    };
    if (!output.valid()) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "scene-linear preview optics found non-finite float RGB samples"
        );
    }
    return output;
}

struct PreparedEditPreviewPixels final {
    Dimensions dimensions;
    std::optional<FloatRgbImage> edited;
    std::vector<std::uint8_t> rgb;
    EditPreviewExecutionReceipt execution;
};

[[nodiscard]] EditPreviewExecutionReceipt edit_preview_execution_receipt(
    const AdjustmentExecutionResult& adjustment,
    const DisplayRgb8Image& display
) {
    if (!adjustment.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "warm preview adjustment stage returned an invalid result"
        );
    }
    if (!display.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "warm preview display stage returned an invalid RGB8 result"
        );
    }
    static_assert(
        edit_preview_cpu_adjustment_backend_version == adjustment_cpu_backend_version
    );
    static_assert(
        edit_preview_metal_adjustment_backend_version == adjustment_metal_backend_version
    );
    static_assert(
        edit_preview_cpu_display_backend_version == display_output_cpu_backend_version
    );
    static_assert(
        edit_preview_metal_display_backend_version == display_output_metal_backend_version
    );
    EditPreviewExecutionReceipt receipt;
    switch (adjustment.backend) {
    case AdjustmentBackend::cpu:
        receipt.adjustment_backend = EditPreviewBackend::cpu;
        receipt.adjustment_backend_version = adjustment_cpu_backend_version;
        break;
    case AdjustmentBackend::metal:
        receipt.adjustment_backend = EditPreviewBackend::metal;
        receipt.adjustment_backend_version = adjustment_metal_backend_version;
        break;
    }
    switch (display.backend) {
    case DisplayOutputBackend::cpu:
        receipt.display_backend = EditPreviewBackend::cpu;
        receipt.display_backend_version = display_output_cpu_backend_version;
        break;
    case DisplayOutputBackend::metal:
        receipt.display_backend = EditPreviewBackend::metal;
        receipt.display_backend_version = display_output_metal_backend_version;
        break;
    }
    receipt.adjustment_fell_back = adjustment.fell_back;
    receipt.display_fell_back = display.fell_back;
    if (!adjustment.diagnostic.empty()) {
        receipt.diagnostic = "adjustment: " + adjustment.diagnostic;
    }
    if (!display.diagnostic.empty()) {
        if (!receipt.diagnostic.empty()) {
            receipt.diagnostic += "; ";
        }
        receipt.diagnostic += "display: " + display.diagnostic;
    }
    if (!receipt.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "warm preview could not describe its effective display backend"
        );
    }
    return receipt;
}

[[nodiscard]] std::optional<PreparedEditPreviewPixels> prepare_edit_preview_pixels(
    const FloatRgbImage& working_proxy,
    const std::shared_ptr<detail::WarmEditGpuSession>& warm_gpu_session,
    const std::string_view warm_gpu_diagnostic,
    const std::span<const AdjustmentNode> nodes,
    const PhotoGeometry& geometry,
    const bool retain_linear_for_analysis,
    const std::stop_token cancellation
) {
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
    const AdjustmentBackendMode backend_mode =
        adjustment_backend_mode_from_environment();
    const bool identity_geometry = geometry == PhotoGeometry{};
    if (backend_mode != AdjustmentBackendMode::cpu && identity_geometry) {
        // Compile before inspecting runtime availability so disabled malformed nodes and source
        // ordering fail identically on every backend.
        const EditExecutionPlan plan = compile_edit_execution_plan(
            nodes,
            working_proxy.level_zero_to_raster_scale_x,
            working_proxy.level_zero_to_raster_scale_y
        );
        if (cancellation.stop_requested()) {
            return std::nullopt;
        }
        std::string diagnostic(warm_gpu_diagnostic);
        if (warm_gpu_session) {
            auto attempt = warm_gpu_session->render(
                nodes,
                plan,
                retain_linear_for_analysis,
                cancellation
            );
            if (attempt.status == detail::WarmEditGpuSession::RenderStatus::cancelled) {
                return std::nullopt;
            }
            if (attempt.status == detail::WarmEditGpuSession::RenderStatus::completed
                && attempt.output.has_value()) {
                auto output = std::move(*attempt.output);
                EditPreviewExecutionReceipt receipt;
                if (output.had_active_adjustments) {
                    receipt.adjustment_backend = EditPreviewBackend::metal;
                    receipt.adjustment_backend_version =
                        edit_preview_metal_adjustment_backend_version;
                }
                receipt.display_backend = EditPreviewBackend::metal;
                receipt.display_backend_version =
                    edit_preview_metal_display_backend_version;
                receipt.fused_pipeline = true;
                if (!receipt.valid()) {
                    throw DecodeError(
                        DecodeErrorCode::internal,
                        0,
                        "session-resident Metal warm preview produced an invalid receipt"
                    );
                }
                return PreparedEditPreviewPixels{
                    .dimensions = output.dimensions,
                    .edited = std::move(output.analyzed_linear),
                    .rgb = std::move(output.rgb8),
                    .execution = std::move(receipt),
                };
            }
            if (cancellation.stop_requested()) {
                return std::nullopt;
            }
            diagnostic = std::move(attempt.diagnostic);
        }
        if (diagnostic.empty()) {
            diagnostic = "session-resident Metal warm preview is unavailable";
        }
        if (backend_mode == AdjustmentBackendMode::metal) {
            throw EditError(
                EditErrorCode::backend_failure,
                std::nullopt,
                std::move(diagnostic)
            );
        }

        // Automatic selection is all-or-nothing at the fused boundary. A declined/failing warm
        // attempt replays adjustment and display completely on the CPU from the immutable host
        // source; it never drops into the old split Metal stages and cannot expose partial data.
        AdjustmentExecutionResult adjustment;
        DisplayRgb8Image display;
        FloatRgbImage geometry_applied;
        try {
            detail::ScopedRowCancellation scoped_cancellation(cancellation);
            detail::throw_if_row_cancelled();
            adjustment = execute_adjustment_nodes_with_backend(
                working_proxy,
                nodes,
                AdjustmentExecutionContext{
                    .full_dimensions = working_proxy.dimensions,
                },
                AdjustmentBackendMode::cpu
            );
            detail::throw_if_row_cancelled();
            geometry_applied = apply_photo_geometry(adjustment.pixels, geometry);
            display = render_linear_srgb_to_display_srgb8_with_backend(
                geometry_applied,
                DisplayOutputRequest{.target_dimensions = geometry_applied.dimensions},
                DisplayOutputBackendMode::cpu
            );
            detail::throw_if_row_cancelled();
        } catch (const detail::RowExecutionCancelled&) {
            return std::nullopt;
        }
        if (cancellation.stop_requested()) {
            return std::nullopt;
        }
        auto receipt = edit_preview_execution_receipt(adjustment, display);
        receipt.adjustment_fell_back = !plan.segments.empty();
        receipt.display_fell_back = true;
        receipt.diagnostic = "warm fused Metal: " + diagnostic;
        if (!receipt.valid()) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "warm-preview complete CPU fallback produced an invalid receipt"
            );
        }
        return PreparedEditPreviewPixels{
            .dimensions = geometry_applied.dimensions,
            .edited = retain_linear_for_analysis
                ? std::optional<FloatRgbImage>{std::move(geometry_applied)}
                : std::nullopt,
            .rgb = std::move(display.bytes),
            .execution = std::move(receipt),
        };
    }

    AdjustmentExecutionResult adjustment;
    DisplayRgb8Image display;
    FloatRgbImage geometry_applied;
    try {
        detail::ScopedRowCancellation scoped_cancellation(cancellation);
        detail::throw_if_row_cancelled();
        adjustment = execute_adjustment_nodes_with_backend(
            working_proxy,
            nodes,
            AdjustmentExecutionContext{
                .full_dimensions = working_proxy.dimensions,
            },
            AdjustmentBackendMode::cpu
        );
        detail::throw_if_row_cancelled();
        geometry_applied = apply_photo_geometry(adjustment.pixels, geometry);
        display = render_linear_srgb_to_display_srgb8_with_backend(
            geometry_applied,
            DisplayOutputRequest{.target_dimensions = geometry_applied.dimensions},
            DisplayOutputBackendMode::cpu
        );
        detail::throw_if_row_cancelled();
    } catch (const detail::RowExecutionCancelled&) {
        return std::nullopt;
    }
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
    if (!identity_geometry && backend_mode != AdjustmentBackendMode::cpu) {
        adjustment.fell_back = true;
        adjustment.diagnostic = "photo geometry currently uses the CPU executor";
    }
    auto execution = edit_preview_execution_receipt(adjustment, display);
    return PreparedEditPreviewPixels{
        .dimensions = geometry_applied.dimensions,
        .edited = retain_linear_for_analysis
            ? std::optional<FloatRgbImage>{std::move(geometry_applied)}
            : std::nullopt,
        .rgb = std::move(display.bytes),
        .execution = std::move(execution),
    };
}

// Local-mask layers deliberately execute on the CPU for now. The existing Metal executor owns
// a flat node stream, while a layer needs a temporary before/after image and a spatial blend.
// Keeping this separate means ordinary unmasked recipes retain the fast path unchanged and the
// later GPU implementation has one clear semantic target to match.
[[nodiscard]] std::optional<PreparedEditPreviewPixels> prepare_edit_preview_layer_pixels(
    const FloatRgbImage& working_proxy,
    const std::span<const AdjustmentLayer> layers,
    const PhotoGeometry& geometry,
    const bool retain_linear_for_analysis,
    const std::stop_token cancellation
) {
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }

    const AdjustmentBackendMode requested_backend =
        adjustment_backend_mode_from_environment();
    const bool cpu_fallback = requested_backend != AdjustmentBackendMode::cpu;

    AdjustmentExecutionResult adjustment;
    DisplayRgb8Image display;
    FloatRgbImage geometry_applied;
    try {
        detail::ScopedRowCancellation scoped_cancellation(cancellation);
        detail::throw_if_row_cancelled();
        adjustment = AdjustmentExecutionResult{
            .pixels = execute_adjustment_layers(
                working_proxy,
                layers,
                AdjustmentExecutionContext{.full_dimensions = working_proxy.dimensions}
            ),
            .backend = AdjustmentBackend::cpu,
            .fell_back = cpu_fallback,
            .diagnostic = cpu_fallback
                ? "local-mask layers currently use the CPU executor"
                : "",
        };
        detail::throw_if_row_cancelled();
        geometry_applied = apply_photo_geometry(adjustment.pixels, geometry);
        display = render_linear_srgb_to_display_srgb8_with_backend(
            geometry_applied,
            DisplayOutputRequest{.target_dimensions = geometry_applied.dimensions},
            DisplayOutputBackendMode::cpu
        );
        detail::throw_if_row_cancelled();
    } catch (const detail::RowExecutionCancelled&) {
        return std::nullopt;
    }
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
    auto execution = edit_preview_execution_receipt(adjustment, display);
    return PreparedEditPreviewPixels{
        .dimensions = geometry_applied.dimensions,
        .edited = retain_linear_for_analysis
            ? std::optional<FloatRgbImage>{std::move(geometry_applied)}
            : std::nullopt,
        .rgb = std::move(display.bytes),
        .execution = std::move(execution),
    };
}

[[nodiscard]] std::optional<EditPreviewAnalysis> analyze_edit_preview(
    const FloatRgbImage& edited,
    const std::vector<std::uint8_t>& rgb,
    const std::stop_token cancellation
) {
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
    if (edited.dimensions.width == 0U || edited.dimensions.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited preview analysis received empty dimensions"
        );
    }
    const std::size_t expected_rgb_size =
        proxy_detail::checked_interleaved_rgb_sample_count(edited.dimensions);
    const std::size_t minimum_row_samples =
        static_cast<std::size_t>(edited.dimensions.width) * 3U;
    if (edited.row_stride_bytes % sizeof(float) != 0U
        || edited.row_stride_bytes / sizeof(float) < minimum_row_samples
        || rgb.size() != expected_rgb_size) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited preview analysis received an invalid RGB layout"
        );
    }
    const std::size_t float_row_stride = edited.row_stride_bytes / sizeof(float);
    if (
        float_row_stride > std::numeric_limits<std::size_t>::max()
            / static_cast<std::size_t>(edited.dimensions.height)
    ) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited preview analysis scene-linear layout overflows"
        );
    }
    const std::size_t required_float_samples =
        float_row_stride * static_cast<std::size_t>(edited.dimensions.height);
    if (required_float_samples > edited.samples.size()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited preview analysis received truncated scene-linear pixels"
        );
    }

    EditPreviewAnalysis analysis;
    analysis.sample_dimensions = edited.dimensions;
    analysis.pixel_count = edited.dimensions.pixel_count();
    for (std::uint32_t y = 0; y < edited.dimensions.height; ++y) {
        if (cancellation.stop_requested()) {
            return std::nullopt;
        }
        for (std::uint32_t x = 0; x < edited.dimensions.width; ++x) {
            const std::size_t rgb_index =
                (static_cast<std::size_t>(y) * edited.dimensions.width + x) * 3U;
            const std::size_t float_index =
                static_cast<std::size_t>(y) * float_row_stride
                + static_cast<std::size_t>(x) * 3U;
            const std::uint8_t red = rgb[rgb_index];
            const std::uint8_t green = rgb[rgb_index + 1U];
            const std::uint8_t blue = rgb[rgb_index + 2U];
            ++analysis.red[red];
            ++analysis.green[green];
            ++analysis.blue[blue];
            ++analysis.luma[display_rgb::rec709_encoded_luma_u8(red, green, blue)];

            bool shadow_clipped = false;
            bool highlight_clipped = false;
            std::array<double, 3U> linear_samples{};
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const float sample = edited.samples[float_index + channel];
                linear_samples[channel] = static_cast<double>(sample);
                if (sample < 0.0F) {
                    ++analysis.below_zero_samples[channel];
                    shadow_clipped = true;
                }
                if (sample > 1.0F) {
                    ++analysis.above_one_samples[channel];
                    highlight_clipped = true;
                }
            }
            analysis.shadow_clipped_pixels += shadow_clipped ? 1U : 0U;
            analysis.highlight_clipped_pixels += highlight_clipped ? 1U : 0U;

            // The warm-preview working space is standardized linear sRGB/Rec.709-D65.  Track
            // only luminance that survives above SDR display white: this establishes a compact
            // HDR-readiness signal without pretending that the RGB8/JPEG preview itself is HDR.
            const double linear_luminance = linear_samples[0] * 0.2126
                + linear_samples[1] * 0.7152
                + linear_samples[2] * 0.0722;
            if (std::isfinite(linear_luminance) && linear_luminance > 1.0) {
                const double headroom_ev = std::log2(linear_luminance);
                const auto bin = static_cast<std::size_t>(std::clamp(
                    static_cast<long long>(std::floor(headroom_ev)),
                    0LL,
                    static_cast<long long>(edit_preview_hdr_headroom_bin_count - 1U)
                ));
                ++analysis.hdr_headroom_bins[bin];
                ++analysis.hdr_headroom_pixels;
                analysis.hdr_peak_headroom_ev = std::max(
                    analysis.hdr_peak_headroom_ev,
                    headroom_ev
                );
            }
        }
    }
    return cancellation.stop_requested()
        ? std::nullopt
        : std::optional<EditPreviewAnalysis>{std::move(analysis)};
}

struct PreparedWarmEditProxy final {
    FloatRgbImage working_proxy;
    RawDevelopmentReceipt raw_development_receipt;
    RawPipelineReceipt raw_pipeline_receipt;
    OpticsProfileReceipt optics_receipt;
    std::optional<SensorClippingMask> sensor_clipping_mask;
};

// LibRaw's `sizes.flip` describes the output raster orientation. Its processed RGB is already
// rotated/flipped into that coordinate system, so a 90-degree orientation must also swap the
// level-zero dimensions used to translate native-pixel radii into a warm preview. Treat unknown
// values conservatively as unrotated metadata; the known dcraw/LibRaw 5/6 values are the two
// transposed cases seen in current providers.
[[nodiscard]] Dimensions oriented_full_dimensions(const AssetMetadata& metadata) noexcept {
    Dimensions dimensions = metadata.image_dimensions;
    if (metadata.orientation == 5 || metadata.orientation == 6) {
        std::swap(dimensions.width, dimensions.height);
    }
    return dimensions;
}

[[nodiscard]] PreparedWarmEditProxy prepare_warm_edit_proxy_from_preview_reference(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    DevelopedSourceReference developed = develop_source_reference(
        session,
        raw_development_plan,
        max_edge,
        raw_pipeline_policy_from_environment()
    );
    DevelopedSourcePixels preview_reference = std::move(developed.source);
    proxy_detail::validate_developed_source(preview_reference);
    // The float working proxy intentionally contains only pixels and scale metadata. Retain the
    // decoder's receipt separately before the RGB conversion so a prepared session can report
    // the exact RAW-development request that created its source raster.
    RawDevelopmentReceipt raw_development_receipt = std::move(developed.raw_development_receipt);
    std::optional<SensorClippingMask> sensor_clipping_mask = std::move(
        developed.sensor_clipping_mask
    );
    const SourceRenderingReceipt source_rendering = std::visit(
        [&](const auto& value) {
            return resolve_source_rendering(value, session.metadata(), developed.pipeline_receipt);
        },
        preview_reference
    );
    const Dimensions target = proxy_dimensions(
        proxy_detail::developed_source_dimensions(preview_reference),
        max_edge
    );
    FloatRgbImage working_proxy = proxy_detail::resize_developed_source_to_working(
        preview_reference,
        target
    );
    // LibRaw may use a half-size demosaic above. Detail-and-effects radii remain expressed in
    // native level-zero pixels, so preserve the relationship to the *oriented* full output
    // dimensions rather than accidentally doubling one axis for a rotated camera frame.
    const Dimensions full_dimensions = oriented_full_dimensions(session.metadata());
    if (full_dimensions.width > 0U && full_dimensions.height > 0U) {
        working_proxy.level_zero_to_raster_scale_x = static_cast<double>(target.width)
            / static_cast<double>(full_dimensions.width);
        working_proxy.level_zero_to_raster_scale_y = static_cast<double>(target.height)
            / static_cast<double>(full_dimensions.height);
    }

    OpticsProfileReceipt receipt;
    if (optics_provider == nullptr) {
        receipt.status = OpticsProfileStatus::disabled;
        receipt.provider_id = "none";
        receipt.provider_version = "none";
    } else if (std::holds_alternative<PixelBuffer>(preview_reference)) {
        auto corrected = optics_provider->correct_reference_rgb(
            working_to_linear_reference(working_proxy),
            session.metadata(),
            optics_settings
        );
        receipt = std::move(corrected.receipt);
        if (corrected.corrected_reference_rgb.has_value()) {
            const double level_zero_scale_x = working_proxy.level_zero_to_raster_scale_x;
            const double level_zero_scale_y = working_proxy.level_zero_to_raster_scale_y;
            const Dimensions corrected_dimensions =
                corrected.corrected_reference_rgb->dimensions;
            DevelopedSourcePixels corrected_source =
                std::move(*corrected.corrected_reference_rgb);
            working_proxy = proxy_detail::resize_developed_source_to_working(
                corrected_source,
                corrected_dimensions
            );
            working_proxy.level_zero_to_raster_scale_x = level_zero_scale_x;
            working_proxy.level_zero_to_raster_scale_y = level_zero_scale_y;
        }
    } else {
        auto corrected = optics_provider->correct_scene_linear_reference(
            working_to_scene_linear_reference(working_proxy),
            session.metadata(),
            optics_settings
        );
        receipt = std::move(corrected.receipt);
        if (corrected.corrected_scene_linear_rgb.has_value()) {
            const double level_zero_scale_x = working_proxy.level_zero_to_raster_scale_x;
            const double level_zero_scale_y = working_proxy.level_zero_to_raster_scale_y;
            const Dimensions corrected_dimensions =
                corrected.corrected_scene_linear_rgb->dimensions;
            DevelopedSourcePixels corrected_source =
                std::move(*corrected.corrected_scene_linear_rgb);
            working_proxy = proxy_detail::resize_developed_source_to_working(
                corrected_source,
                corrected_dimensions
            );
            working_proxy.level_zero_to_raster_scale_x = level_zero_scale_x;
            working_proxy.level_zero_to_raster_scale_y = level_zero_scale_y;
        }
    }
    apply_source_rendering(working_proxy, source_rendering);
    // Optical providers currently retain preview raster geometry. If an adapter ever returns a
    // different extent, a pre-warp sensor map would be misleading; omit it instead of stretching
    // it or reopening the RAW source just for diagnostics.
    if (
        sensor_clipping_mask.has_value()
        && sensor_clipping_mask->dimensions != working_proxy.dimensions
    ) {
        sensor_clipping_mask.reset();
    }
    return {
        .working_proxy = std::move(working_proxy),
        .raw_development_receipt = std::move(raw_development_receipt),
        .raw_pipeline_receipt = std::move(developed.pipeline_receipt),
        .optics_receipt = std::move(receipt),
        .sensor_clipping_mask = std::move(sensor_clipping_mask),
    };
}

} // namespace

bool EditPreviewExecutionReceipt::valid() const noexcept {
    const auto valid_adjustment_backend = [](
        const EditPreviewBackend backend,
        const std::uint32_t version
    ) {
        switch (backend) {
        case EditPreviewBackend::cpu:
            return version == edit_preview_cpu_adjustment_backend_version;
        case EditPreviewBackend::metal:
            return version == edit_preview_metal_adjustment_backend_version;
        }
        return false;
    };
    const auto valid_display_backend = [](
        const EditPreviewBackend backend,
        const std::uint32_t version
    ) {
        switch (backend) {
        case EditPreviewBackend::cpu:
            return version == edit_preview_cpu_display_backend_version;
        case EditPreviewBackend::metal:
            return version == edit_preview_metal_display_backend_version;
        }
        return false;
    };
    return schema_version == edit_preview_execution_receipt_schema_version
        && valid_adjustment_backend(adjustment_backend, adjustment_backend_version)
        && adjustment_execution_contract_version == edit_execution_plan_identity_version
        && valid_display_backend(display_backend, display_backend_version)
        && display_output_contract_version == display_srgb8_output_transform_version
        && (!adjustment_fell_back || adjustment_backend == EditPreviewBackend::cpu)
        && (!display_fell_back || display_backend == EditPreviewBackend::cpu)
        && (!fused_pipeline || display_backend == EditPreviewBackend::metal)
        && (!fused_pipeline || (!adjustment_fell_back && !display_fell_back))
        && ((adjustment_fell_back || display_fell_back) == !diagnostic.empty());
}

std::string edit_preview_execution_receipt_identity(
    const EditPreviewExecutionReceipt& receipt
) {
    if (!receipt.valid()) {
        throw std::invalid_argument("edit-preview execution receipt is invalid");
    }
    const auto backend_identity = [](const EditPreviewBackend backend) {
        switch (backend) {
        case EditPreviewBackend::cpu:
            return std::string_view{"cpu"};
        case EditPreviewBackend::metal:
            return std::string_view{"metal"};
        }
        throw std::invalid_argument("edit-preview execution backend is invalid");
    };
    return "shadow-edit-preview-execution-v1;adjustment="
        + std::string(backend_identity(receipt.adjustment_backend))
        + "-v" + std::to_string(receipt.adjustment_backend_version)
        + ";plan=" + std::to_string(receipt.adjustment_execution_contract_version)
        + ";display=" + std::string(backend_identity(receipt.display_backend))
        + "-v" + std::to_string(receipt.display_backend_version)
        + ";display-contract=" + std::to_string(receipt.display_output_contract_version)
        + ";route=" + (receipt.fused_pipeline ? "fused" : "staged");
}

std::string edit_preview_generator_implementation_identity() {
    // This identity names the implementations the current generator can actually select, not a
    // local device. Runtime availability and fallback diagnostics remain on each receipt.
    return "shadow-edit-preview-generator-v1;plan="
        + std::to_string(edit_execution_plan_identity_version)
        + ";adjustment-cpu=" + std::string(
            adjustment_backend_identity(AdjustmentBackend::cpu)
        )
        + ";adjustment-metal=" + std::string(
            adjustment_backend_identity(AdjustmentBackend::metal)
        )
        + ";display-cpu=" + std::string(
            display_output_backend_identity(DisplayOutputBackend::cpu)
        )
        + ";display-metal=" + std::string(
            display_output_backend_identity(DisplayOutputBackend::metal)
        )
        + ";warm-fused-metal=v1;features=resident-source,double-slot,"
            "immutable-color-resources,technical-detail,texture,clarity,optics,"
            "adjustment,display"
        + ";display-contract=" + std::to_string(display_srgb8_output_transform_version)
        + ";jpeg-444=" + std::to_string(edit_preview_jpeg_444_contract_version);
}

WarmEditPreviewSession::WarmEditPreviewSession(
    FloatRgbImage working_proxy,
    const std::uint32_t max_edge,
    RawDevelopmentReceipt raw_development_receipt,
    RawPipelineReceipt raw_pipeline_receipt,
    OpticsProfileReceipt optics_receipt,
    std::optional<SensorClippingMask> sensor_clipping_mask
)
    : working_proxy_(std::move(working_proxy)), max_edge_(max_edge),
      raw_development_receipt_(std::move(raw_development_receipt)),
      raw_pipeline_receipt_(std::move(raw_pipeline_receipt)),
      optics_receipt_(std::move(optics_receipt)),
      sensor_clipping_mask_(std::move(sensor_clipping_mask)) {
    auto gpu = detail::prepare_warm_edit_gpu_session(working_proxy_);
    warm_gpu_session_ = std::move(gpu.session);
    warm_gpu_diagnostic_ = std::move(gpu.diagnostic);
}

Dimensions WarmEditPreviewSession::dimensions() const noexcept {
    return working_proxy_.dimensions;
}

std::uint32_t WarmEditPreviewSession::max_edge() const noexcept {
    return max_edge_;
}

const RawDevelopmentReceipt& WarmEditPreviewSession::raw_development_receipt() const noexcept {
    return raw_development_receipt_;
}

const RawPipelineReceipt& WarmEditPreviewSession::raw_pipeline_receipt() const noexcept {
    return raw_pipeline_receipt_;
}

const OpticsProfileReceipt& WarmEditPreviewSession::optics_receipt() const noexcept {
    return optics_receipt_;
}

const std::optional<SensorClippingMask>& WarmEditPreviewSession::sensor_clipping_mask() const
    noexcept {
    return sensor_clipping_mask_;
}

WarmEditPreviewGpuStats WarmEditPreviewSession::gpu_stats() const noexcept {
    return warm_gpu_session_ ? warm_gpu_session_->stats() : WarmEditPreviewGpuStats{};
}

EncodedProxy WarmEditPreviewSession::render_jpeg(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality,
    const PhotoGeometry& geometry
) const {
    auto rendered = render_jpeg_cancellable(nodes, jpeg_quality, {}, geometry);
    if (rendered.cancelled()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable warm preview was unexpectedly cancelled"
        );
    }
    return std::move(*rendered.completed);
}

AnalyzedEditPreview WarmEditPreviewSession::render_jpeg_with_analysis(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality,
    const PhotoGeometry& geometry
) const {
    auto rendered = render_jpeg_with_analysis_cancellable(nodes, jpeg_quality, {}, geometry);
    if (rendered.cancelled()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable analyzed warm preview was unexpectedly cancelled"
        );
    }
    return std::move(*rendered.completed);
}

EncodedProxy WarmEditPreviewSession::render_jpeg_layers(
    const std::span<const AdjustmentLayer> layers,
    const std::uint8_t jpeg_quality,
    const PhotoGeometry& geometry
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_layer_pixels(working_proxy_, layers, geometry, false, {});
    if (!prepared.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable local-mask warm preview was unexpectedly cancelled"
        );
    }
    return EncodedProxy{
        .dimensions = prepared->dimensions,
        .bytes = proxy_detail::encode_proxy_jpeg(prepared->rgb, prepared->dimensions, jpeg_quality),
    };
}

AnalyzedEditPreview WarmEditPreviewSession::render_jpeg_with_analysis_layers(
    const std::span<const AdjustmentLayer> layers,
    const std::uint8_t jpeg_quality,
    const PhotoGeometry& geometry
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_layer_pixels(working_proxy_, layers, geometry, true, {});
    if (!prepared.has_value() || !prepared->edited.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "local-mask analyzed warm preview did not retain its scene-linear result"
        );
    }
    auto analysis = analyze_edit_preview(*prepared->edited, prepared->rgb, {});
    if (!analysis.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable local-mask preview analysis was unexpectedly cancelled"
        );
    }
    return AnalyzedEditPreview{
        .proxy = EncodedProxy{
            .dimensions = prepared->dimensions,
            .bytes = proxy_detail::encode_proxy_jpeg(
                prepared->rgb,
                prepared->dimensions,
                jpeg_quality
            ),
        },
        .analysis = std::move(*analysis),
        .execution = std::move(prepared->execution),
    };
}

CancellableEditPreviewResult<EncodedProxy>
WarmEditPreviewSession::render_jpeg_cancellable(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        nodes,
        geometry,
        false,
        cancellation
    );
    if (!prepared.has_value()) {
        return {};
    }
    auto encoded = proxy_detail::encode_proxy_jpeg_cancellable(
        prepared->rgb,
        prepared->dimensions,
        jpeg_quality,
        cancellation
    );
    if (!encoded.has_value()) {
        return {};
    }
    return {
        .completed = EncodedProxy{
            .dimensions = prepared->dimensions,
            .bytes = std::move(*encoded),
        },
    };
}

CancellableEditPreviewResult<AnalyzedEditPreview>
WarmEditPreviewSession::render_jpeg_with_analysis_cancellable(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        nodes,
        geometry,
        true,
        cancellation
    );
    if (!prepared.has_value()) {
        return {};
    }
    if (!prepared->edited.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "analyzed warm preview did not retain its scene-linear result"
        );
    }
    auto analysis = analyze_edit_preview(
        *prepared->edited,
        prepared->rgb,
        cancellation
    );
    if (!analysis.has_value()) {
        return {};
    }
    auto encoded = proxy_detail::encode_proxy_jpeg_cancellable(
        prepared->rgb,
        prepared->dimensions,
        jpeg_quality,
        cancellation
    );
    if (!encoded.has_value()) {
        return {};
    }

    EncodedProxy proxy;
    proxy.dimensions = prepared->dimensions;
    proxy.bytes = std::move(*encoded);
    return {
        .completed = AnalyzedEditPreview{
            .proxy = std::move(proxy),
            .analysis = std::move(*analysis),
            .execution = std::move(prepared->execution),
        },
    };
}

WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    return prepare_warm_edit_preview(
        session,
        max_edge,
        preview_raw_development_plan(),
        optics_provider,
        optics_settings
    );
}

WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    validate_warm_edit_max_edge(max_edge);
    proxy_detail::validate_raw_development_plan_intent(
        raw_development_plan,
        RawDevelopmentIntent::preview,
        "warm edit preview"
    );
    auto prepared = prepare_warm_edit_proxy_from_preview_reference(
        session,
        max_edge,
        raw_development_plan,
        optics_provider,
        optics_settings
    );
    return WarmEditPreviewSession(
        std::move(prepared.working_proxy),
        max_edge,
        std::move(prepared.raw_development_receipt),
        std::move(prepared.raw_pipeline_receipt),
        std::move(prepared.optics_receipt),
        std::move(prepared.sensor_clipping_mask)
    );
}

} // namespace shadow::image
