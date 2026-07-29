#include "edit_preview_rendering.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/working_rgb.hpp>

#include "../concurrency/row_scheduler.hpp"
#include "../edit/local_mask_validation.hpp"
#include "developed_source_raster.hpp"
#include "display_rgb_math.hpp"
#include "warm_edit_gpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::image::edit_preview_detail {

namespace {

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
    static_assert(edit_preview_cpu_adjustment_backend_version == adjustment_cpu_backend_version);
    static_assert(
        edit_preview_metal_adjustment_backend_version == adjustment_metal_backend_version
    );
    static_assert(edit_preview_cpu_display_backend_version == display_output_cpu_backend_version);
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

} // namespace

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
    const AdjustmentBackendMode backend_mode = adjustment_backend_mode_from_environment();
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
            auto attempt =
                warm_gpu_session->render(nodes, plan, retain_linear_for_analysis, cancellation);
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
                receipt.display_backend_version = edit_preview_metal_display_backend_version;
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
            throw EditError(EditErrorCode::backend_failure, std::nullopt, std::move(diagnostic));
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

[[nodiscard]] std::optional<PreparedEditPreviewPixels> prepare_edit_preview_layer_pixels(
    const FloatRgbImage& working_proxy,
    const std::shared_ptr<detail::WarmEditGpuSession>& warm_gpu_session,
    const std::string_view warm_gpu_diagnostic,
    const std::span<const AdjustmentLayer> layers,
    const PhotoGeometry& geometry,
    const bool retain_linear_for_analysis,
    const std::stop_token cancellation
) {
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }

    const AdjustmentBackendMode backend_mode = adjustment_backend_mode_from_environment();
    const bool identity_geometry = geometry == PhotoGeometry{};
    static_cast<void>(detail::validate_adjustment_layer_plan(
        working_proxy,
        layers,
        AdjustmentExecutionContext{.full_dimensions = working_proxy.dimensions}
    ));
    std::string fallback_diagnostic;
    if (backend_mode != AdjustmentBackendMode::cpu && identity_geometry) {
        fallback_diagnostic = std::string(warm_gpu_diagnostic);
        if (warm_gpu_session) {
            auto attempt =
                warm_gpu_session->render_layers(layers, retain_linear_for_analysis, cancellation);
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
                receipt.display_backend_version = edit_preview_metal_display_backend_version;
                receipt.fused_pipeline = true;
                if (!receipt.valid()) {
                    throw DecodeError(
                        DecodeErrorCode::internal,
                        0,
                        "session-resident Metal layer preview produced an invalid receipt"
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
            fallback_diagnostic = std::move(attempt.diagnostic);
        }
        if (fallback_diagnostic.empty()) {
            fallback_diagnostic = "session-resident Metal layer preview is unavailable";
        }
        if (backend_mode == AdjustmentBackendMode::metal) {
            throw EditError(EditErrorCode::backend_failure, std::nullopt, fallback_diagnostic);
        }
    } else if (!identity_geometry && backend_mode != AdjustmentBackendMode::cpu) {
        fallback_diagnostic = "photo geometry currently uses the CPU layer executor";
        if (backend_mode == AdjustmentBackendMode::metal) {
            throw EditError(EditErrorCode::backend_failure, std::nullopt, fallback_diagnostic);
        }
    }

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
            .fell_back = !fallback_diagnostic.empty(),
            .diagnostic = fallback_diagnostic.empty() ? std::string{}
                                                      : "warm fused Metal: " + fallback_diagnostic,
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
    if (!fallback_diagnostic.empty()) {
        // The resident layer route is one transaction. If it declines, both edit and display
        // restart from the immutable host source; the receipt must describe that atomic replay
        // just as the ordinary-node fused route does.
        execution.adjustment_fell_back = true;
        execution.display_fell_back = true;
        execution.diagnostic = "warm fused Metal: " + fallback_diagnostic;
    }
    if (!execution.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "warm-preview layer CPU fallback produced an invalid receipt"
        );
    }
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
    const std::size_t minimum_row_samples = static_cast<std::size_t>(edited.dimensions.width) * 3U;
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
    if (float_row_stride > std::numeric_limits<std::size_t>::max()
                               / static_cast<std::size_t>(edited.dimensions.height)) {
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
                static_cast<std::size_t>(y) * float_row_stride + static_cast<std::size_t>(x) * 3U;
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
            const double linear_luminance = linear_samples[0] * 0.2126 + linear_samples[1] * 0.7152
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
                analysis.hdr_peak_headroom_ev =
                    std::max(analysis.hdr_peak_headroom_ev, headroom_ev);
            }
        }
    }
    return cancellation.stop_requested() ? std::nullopt
                                         : std::optional<EditPreviewAnalysis>{std::move(analysis)};
}

} // namespace shadow::image::edit_preview_detail
