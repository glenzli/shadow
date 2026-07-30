#include "raw_foundation_source.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image::raw_pipeline_detail {

namespace {

inline constexpr std::string_view raw_foundation_pipeline_identity =
    "shadow-raw-foundation-developer-v1:verified-linear-camera-rgb:"
    "camera-matrix:scene-linear-f32";

[[nodiscard]] RawDevelopmentReceipt finalize_raw_foundation_receipt(
    const PreparedRawFrameDevelopment& prepared,
    const RawDevelopmentPlan& requested_plan,
    const RawDevelopmentPlanNegotiationStatus negotiation_status,
    const Dimensions rendered_dimensions,
    const bool bounded_preview,
    const std::string_view foundation_cache_identity,
    const DcpColorExecutionBackend dcp_execution_backend
) {
    const RawFrameDescriptor& descriptor = prepared.descriptor();
    const RawDevelopmentPlan& effective_plan = prepared.development_plan();
    const DcpColorTransform* camera_profile = prepared.camera_profile();

    RawDevelopmentReceipt receipt;
    receipt.schema_version = raw_development_receipt_schema_version;
    receipt.provider_id =
        descriptor.provider_id.empty() ? "provider-neutral-raw-frame" : descriptor.provider_id;
    receipt.provider_version =
        descriptor.provider_version.empty() ? "unrecorded" : descriptor.provider_version;
    receipt.development_settings_signature =
        "shadow-raw-foundation-v1;reconstruction=rawnind-public-bayer;"
        "backend=cpu;highlight=disabled;conventional-raw-denoise=disabled;";
    receipt.development_settings_signature += foundation_cache_identity;
    if (camera_profile != nullptr) {
        receipt.development_settings_signature +=
            ";color=dcp;" + dcp_color_receipt_identity(camera_profile->receipt);
        receipt.development_settings_signature +=
            ";" + std::string(dcp_color_execution_backend_identity(dcp_execution_backend));
    } else {
        receipt.development_settings_signature +=
            effective_plan.white_balance.mode == RawWhiteBalanceMode::as_shot
                ? ";wb=as-shot;matrix=provider-generic"
                : ";wb=camera-neutral;matrix=provider-generic";
    }
    receipt.requested_plan = requested_plan;
    receipt.requested_plan_identity = raw_development_plan_identity(requested_plan);
    receipt.effective_plan = effective_plan;
    receipt.effective_plan_identity = raw_development_plan_identity(effective_plan);
    receipt.plan_negotiation_status = negotiation_status;
    receipt.processed_linear_reference_contract_version =
        processed_linear_reference_rgb_contract_version;
    receipt.declared_image_dimensions = descriptor.active_dimensions;
    receipt.rendered_dimensions = rendered_dimensions;
    receipt.orientation = descriptor.orientation;
    receipt.half_size = bounded_preview;
    receipt.use_camera_white_balance = true;
    receipt.use_camera_matrix = true;
    receipt.use_auto_brightness = false;
    receipt.use_exposure_correction = false;
    receipt.brightness = 1.0F;
    receipt.maximum_adjustment_threshold = 0.0F;
    receipt.output_bits_per_channel = 32U;
    // This field mirrors a LibRaw numeric selector for compatibility receipts. The AI
    // reconstruction is named completely by `development_settings_signature`, so -1 is the
    // deliberate not-applicable value.
    receipt.demosaic_quality = -1;
    receipt.output_color = 1;
    receipt.gamma_inverse_power = 1.0;
    receipt.gamma_linear_toe_slope = 1.0;
    receipt.declared_dng_opcode_lists = descriptor.declared_pending_corrections;
    for (std::size_t index = 0U; index < receipt.dng_opcode_execution.size(); ++index) {
        receipt.dng_opcode_execution[index] =
            descriptor.declared_pending_corrections.dng_opcode_list_bytes[index] == 0U
                ? DngOpcodeExecutionStatus::not_declared
                : DngOpcodeExecutionStatus::unsupported;
    }
    return receipt;
}

} // namespace

DevelopedSourceReference materialize_prepared_raw_foundation_source(
    PreparedRawFrameSource prepared,
    const RawFoundationCameraRgbView& foundation,
    const RawDevelopmentPlan& requested_plan
) {
    const RawDevelopmentPlan& effective_plan = prepared.development_.development_plan();
    const RawDevelopmentPlanNegotiationStatus negotiation_status =
        requested_plan == effective_plan ? RawDevelopmentPlanNegotiationStatus::accepted
                                         : RawDevelopmentPlanNegotiationStatus::adjusted;
    if (!foundation.matches_source(prepared.development_.descriptor())) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "verified AI RAW foundation does not match the decoded source geometry"
        );
    }

    RawPipelineReceipt pipeline = std::move(prepared.pipeline_);
    pipeline.requested_plan = requested_plan;
    pipeline.effective_plan = effective_plan;
    DevelopedRawFoundation developed = develop_raw_foundation(
        foundation,
        prepared.development_.descriptor(),
        prepared.development_.linear_transform(),
        prepared.development_.preview_max_edge()
    );
    SensorClippingMask sensor_clipping =
        project_sensor_clipping_mask(prepared.frame_, developed.scene_linear.dimensions);

    DcpColorExecutionBackend dcp_execution_backend = DcpColorExecutionBackend::cpu;
    const DcpColorTransform* camera_profile = prepared.development_.camera_profile();
    if (camera_profile != nullptr && camera_profile->has_post_matrix_stages()) {
        dcp_execution_backend =
            apply_dcp_color_rendering_stages(developed.scene_linear, *camera_profile);
    }

    RawDevelopmentReceipt raw_receipt = finalize_raw_foundation_receipt(
        prepared.development_,
        requested_plan,
        negotiation_status,
        developed.scene_linear.dimensions,
        developed.bounded_preview,
        developed.cache_identity,
        dcp_execution_backend
    );
    pipeline = finalize_raw_frame_pipeline_receipt(
        std::move(pipeline),
        RawDevelopmentBackend::cpu,
        effective_plan.highlight_recovery,
        developed.cache_identity,
        raw_foundation_pipeline_identity
    );
    return DevelopedSourceReference{
        .source = std::move(developed.scene_linear),
        .raw_development_receipt = std::move(raw_receipt),
        .pipeline_receipt = std::move(pipeline),
        .sensor_clipping_mask = std::move(sensor_clipping),
    };
}

} // namespace shadow::image::raw_pipeline_detail
