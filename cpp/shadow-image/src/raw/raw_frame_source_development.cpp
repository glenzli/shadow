#include "raw_frame_source_development.hpp"

#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_denoise.hpp>

#include "metal_raw_development.hpp"
#include "raw_denoise_plan.hpp"

#include <optional>
#include <utility>

namespace shadow::image::raw_pipeline_detail {

DevelopedRawFrame develop_raw_frame(PreparedRawFrameSource& prepared_source) {
    RawFrame frame = std::move(prepared_source.frame_);
    PreparedRawFrameDevelopment prepared = std::move(prepared_source.development_);
    std::optional<SensorClippingMask> sensor_clipping_mask;
    HighlightChromaRiskMap highlight_chroma_risk_map = project_highlight_chroma_risk_map(
        frame,
        prepared.diagnostic_dimensions(),
        prepared.development_plan().highlight_recovery == RawHighlightRecoveryIntent::aggressive
    );
    std::optional<FusedRawFrameDevelopment> prepared_development;
    std::optional<RawBayerDenoiseResult> materialized_raw_denoise;
    RawBayerDenoiseReceipt raw_denoise_receipt;
    bool fused_dcp_applied = false;
    const DcpColorTransform* camera_profile = prepared.camera_profile();
    const bool dcp_requested =
        camera_profile != nullptr && camera_profile->has_post_matrix_stages();
    if (prepared.requested_backend() != RawDevelopmentBackendMode::cpu) {
        auto fused_attempt = detail::try_develop_bayer_linear_srgb_f32_metal(
            frame,
            prepared.linear_transform(),
            prepared.preview_max_edge(),
            prepared.development_plan().highlight_recovery,
            prepared.development_plan().quality,
            detail::MetalRawDevelopmentContinuations{
                .dcp_color_transform = dcp_requested ? camera_profile : nullptr,
                .raw_denoise = prepared.raw_denoise().applied() ? &prepared.raw_denoise() : nullptr,
                .project_sensor_clipping = !sensor_clipping_mask.has_value(),
            }
        );
        const bool clipping_available =
            sensor_clipping_mask.has_value() || fused_attempt.sensor_clipping_mask.has_value();
        if (fused_attempt.development.has_value() && clipping_available
            && fused_attempt.raw_denoise_applied == prepared.raw_denoise().applied()
            && fused_attempt.dcp_applied == dcp_requested) {
            prepared_development = std::move(fused_attempt.development);
            if (!sensor_clipping_mask.has_value()) {
                sensor_clipping_mask = std::move(fused_attempt.sensor_clipping_mask);
            }
            fused_dcp_applied = fused_attempt.dcp_applied;
            raw_denoise_receipt = detail::finalize_raw_bayer_denoise_receipt(
                frame,
                prepared.raw_denoise(),
                prepared.raw_denoise().applied() ? RawBayerDenoiseBackend::metal
                                                 : RawBayerDenoiseBackend::cpu
            );
        }
    }
    if (!prepared_development.has_value()) {
        if (!sensor_clipping_mask.has_value()) {
            sensor_clipping_mask =
                project_sensor_clipping_mask(frame, prepared.diagnostic_dimensions());
        }
        materialized_raw_denoise =
            detail::execute_prepared_raw_bayer_denoise(std::move(frame), prepared.raw_denoise());
        raw_denoise_receipt = materialized_raw_denoise->receipt;
        prepared_development = develop_bayer_linear_srgb_f32_fused(
            materialized_raw_denoise->frame,
            prepared.linear_transform(),
            prepared.preview_max_edge(),
            prepared.development_plan().highlight_recovery,
            prepared.development_plan().quality
        );
    }
    FusedRawFrameDevelopment developed = std::move(*prepared_development);
    const Dimensions rendered_dimensions = developed.scene_linear.dimensions;
    DevelopedSourcePixels output = std::move(developed.scene_linear);
    DcpColorExecutionBackend dcp_execution_backend =
        fused_dcp_applied ? DcpColorExecutionBackend::metal : DcpColorExecutionBackend::cpu;
    if (camera_profile != nullptr && camera_profile->has_post_matrix_stages()) {
        // DCP's HueSatMap/LookTable/ProfileToneCurve define input rendering. They intentionally
        // run before the Recipe graph and stay in the camera-development receipt.
        if (!fused_dcp_applied) {
            dcp_execution_backend = apply_dcp_color_rendering_stages(
                std::get<SceneLinearRgbFrame>(output),
                *camera_profile
            );
        }
    }
    RawDevelopmentReceipt receipt = finalize_raw_frame_development_receipt(
        prepared,
        rendered_dimensions,
        developed.demosaic_receipt,
        developed.backend,
        raw_denoise_receipt,
        dcp_execution_backend
    );
    return DevelopedRawFrame{
        .source = std::move(output),
        .raw_development_receipt = std::move(receipt),
        .sensor_clipping_mask = std::move(*sensor_clipping_mask),
        .highlight_chroma_risk_map = std::move(highlight_chroma_risk_map),
        .backend = developed.backend,
        .highlight_recovery = developed.highlight_recovery,
        .raw_denoise_cache_identity = raw_denoise_receipt.cache_identity,
        .source_scene_luminance_percentile = prepared.source_scene_luminance_percentile(),
    };
}

} // namespace shadow::image::raw_pipeline_detail
