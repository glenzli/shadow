#include <shadow/image/cxx_bridge.hpp>

#include <shadow/image/edit.hpp>
#include <shadow/image/display_luma.hpp>
#include <shadow/image/sensor_clipping.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/source_profile_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::bridge {

namespace {

[[nodiscard]] FfiDimensions dimensions(const image::Dimensions value) noexcept {
    return FfiDimensions{value.width, value.height};
}

[[nodiscard]] FfiPreviewFormat preview_format(const image::PreviewFormat value) noexcept {
    switch (value) {
    case image::PreviewFormat::jpeg:
        return FfiPreviewFormat::Jpeg;
    case image::PreviewFormat::bitmap:
        return FfiPreviewFormat::Bitmap;
    case image::PreviewFormat::jpeg_xl:
        return FfiPreviewFormat::JpegXl;
    case image::PreviewFormat::h265:
        return FfiPreviewFormat::H265;
    case image::PreviewFormat::unknown:
        return FfiPreviewFormat::Unknown;
    }
    return FfiPreviewFormat::Unknown;
}

[[nodiscard]] FfiByteOrder byte_order(const image::ByteOrder value) noexcept {
    switch (value) {
    case image::ByteOrder::native:
        return FfiByteOrder::Native;
    case image::ByteOrder::little_endian:
        return FfiByteOrder::LittleEndian;
    case image::ByteOrder::big_endian:
        return FfiByteOrder::BigEndian;
    case image::ByteOrder::not_applicable:
        return FfiByteOrder::NotApplicable;
    }
    return FfiByteOrder::NotApplicable;
}

[[nodiscard]] FfiPreviewSnapshot preview_snapshot(const image::PreviewDescriptor& preview) {
    return FfiPreviewSnapshot{
        preview.id,
        preview_format(preview.format),
        dimensions(preview.dimensions),
        preview.bits_per_channel,
        preview.channels,
        preview.encoded_bytes,
        preview.decodable,
    };
}

[[nodiscard]] FfiEncodedProxy encoded_proxy(const image::EncodedProxy& proxy) {
    FfiEncodedProxy result;
    result.dimensions = dimensions(proxy.dimensions);
    result.format = preview_format(proxy.format);
    result.bits_per_channel = proxy.bits_per_channel;
    result.channels = proxy.channels;
    result.bytes.reserve(proxy.bytes.size());
    for (const auto byte : proxy.bytes) {
        result.bytes.push_back(byte);
    }
    return result;
}

[[nodiscard]] FfiSensorClippingMask ffi_sensor_clipping_mask(
    const image::SensorClippingMask& mask
) {
    FfiSensorClippingMask result;
    result.available = true;
    result.dimensions = dimensions(mask.dimensions);
    result.highlight_pixel_count = mask.highlight_pixel_count;
    result.shadow_pixel_count = mask.shadow_pixel_count;
    result.samples.reserve(mask.samples.size());
    for (const auto sample : mask.samples) {
        result.samples.push_back(sample);
    }
    return result;
}

template <std::size_t Size>
[[nodiscard]] rust::Vec<std::uint64_t> sample_counts(
    const std::array<std::uint64_t, Size>& source
) {
    rust::Vec<std::uint64_t> result;
    result.reserve(source.size());
    for (const auto count : source) {
        result.push_back(count);
    }
    return result;
}

[[nodiscard]] FfiEditPreviewAnalysis edit_preview_analysis(
    const image::EditPreviewAnalysis& analysis
) {
    FfiEditPreviewAnalysis result;
    result.version = rust::String(
        image::edit_preview_analysis_version.data(),
        image::edit_preview_analysis_version.size()
    );
    result.sample_dimensions = dimensions(analysis.sample_dimensions);
    result.red = sample_counts(analysis.red);
    result.green = sample_counts(analysis.green);
    result.blue = sample_counts(analysis.blue);
    result.luma = sample_counts(analysis.luma);
    result.below_zero_samples = sample_counts(analysis.below_zero_samples);
    result.above_one_samples = sample_counts(analysis.above_one_samples);
    result.pixel_count = analysis.pixel_count;
    result.shadow_clipped_pixels = analysis.shadow_clipped_pixels;
    result.highlight_clipped_pixels = analysis.highlight_clipped_pixels;
    return result;
}

[[nodiscard]] FfiEditPreviewBackend edit_preview_backend(
    const image::EditPreviewBackend backend
) {
    switch (backend) {
    case image::EditPreviewBackend::cpu:
        return FfiEditPreviewBackend::Cpu;
    case image::EditPreviewBackend::metal:
        return FfiEditPreviewBackend::Metal;
    }
    throw image::DecodeError(
        image::DecodeErrorCode::internal,
        0,
        "edit-preview execution receipt contains an invalid backend"
    );
}

[[nodiscard]] FfiEditPreviewExecutionReceipt edit_preview_execution_receipt(
    const image::EditPreviewExecutionReceipt& receipt
) {
    if (!receipt.valid()) {
        throw image::DecodeError(
            image::DecodeErrorCode::internal,
            0,
            "edit-preview execution receipt is invalid"
        );
    }
    FfiEditPreviewExecutionReceipt result;
    result.schema_version = receipt.schema_version;
    result.cache_identity = rust::String(
        image::edit_preview_execution_receipt_identity(receipt)
    );
    result.adjustment_backend = edit_preview_backend(receipt.adjustment_backend);
    result.adjustment_backend_version = receipt.adjustment_backend_version;
    result.adjustment_execution_contract_version =
        receipt.adjustment_execution_contract_version;
    result.display_backend = edit_preview_backend(receipt.display_backend);
    result.display_backend_version = receipt.display_backend_version;
    result.display_output_contract_version = receipt.display_output_contract_version;
    result.adjustment_fell_back = receipt.adjustment_fell_back;
    result.display_fell_back = receipt.display_fell_back;
    result.diagnostic = rust::String(receipt.diagnostic);
    return result;
}

[[nodiscard]] FfiAnalyzedEditPreview analyzed_edit_preview(
    const image::AnalyzedEditPreview& preview
) {
    FfiAnalyzedEditPreview result;
    result.proxy = encoded_proxy(preview.proxy);
    result.analysis = edit_preview_analysis(preview.analysis);
    result.execution = edit_preview_execution_receipt(preview.execution);
    return result;
}

[[nodiscard]] FfiDetailTileRect detail_tile_rect(const image::DetailTileRect value) noexcept {
    return FfiDetailTileRect{value.x, value.y, value.width, value.height};
}

[[nodiscard]] image::DetailTileRect detail_tile_rect(const FfiDetailTileRect& value) noexcept {
    return image::DetailTileRect{value.x, value.y, value.width, value.height};
}

[[nodiscard]] image::PhotoGeometry photo_geometry(const FfiPhotoGeometry& value) {
    image::PhotoQuarterTurn quarter_turn = image::PhotoQuarterTurn::zero;
    switch (value.quarter_turn) {
    case 0U:
        quarter_turn = image::PhotoQuarterTurn::zero;
        break;
    case 1U:
        quarter_turn = image::PhotoQuarterTurn::clockwise_90;
        break;
    case 2U:
        quarter_turn = image::PhotoQuarterTurn::clockwise_180;
        break;
    case 3U:
        quarter_turn = image::PhotoQuarterTurn::clockwise_270;
        break;
    default:
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "photo geometry uses an unsupported quarter-turn"
        );
    }
    const image::PhotoGeometry geometry{
        .crop_left = value.crop_left,
        .crop_top = value.crop_top,
        .crop_right = value.crop_right,
        .crop_bottom = value.crop_bottom,
        .quarter_turn = quarter_turn,
        .flip_horizontal = value.flip_horizontal,
        .flip_vertical = value.flip_vertical,
    };
    image::validate_photo_geometry(geometry);
    return geometry;
}

[[nodiscard]] FfiRenderedDetailTile rendered_detail_tile(
    const image::RenderedDetailTile& tile
) {
    FfiRenderedDetailTile result;
    result.rect = detail_tile_rect(tile.rect);
    result.full_dimensions = dimensions(tile.full_dimensions);
    result.row_stride_bytes = tile.row_stride_bytes;
    result.bytes.reserve(tile.bytes.size());
    for (const auto byte : tile.bytes) {
        result.bytes.push_back(byte);
    }
    return result;
}

[[nodiscard]] rust::String optics_status(const image::OpticsProfileStatus status) {
    switch (status) {
    case image::OpticsProfileStatus::disabled:
        return rust::String("disabled");
    case image::OpticsProfileStatus::provider_unavailable:
        return rust::String("provider_unavailable");
    case image::OpticsProfileStatus::insufficient_metadata:
        return rust::String("insufficient_metadata");
    case image::OpticsProfileStatus::camera_not_found:
        return rust::String("camera_not_found");
    case image::OpticsProfileStatus::lens_not_found:
        return rust::String("lens_not_found");
    case image::OpticsProfileStatus::incompatible_input:
        return rust::String("incompatible_input");
    case image::OpticsProfileStatus::matched:
        return rust::String("matched");
    }
    return rust::String("provider_unavailable");
}

[[nodiscard]] FfiOpticsReceipt optics_receipt(const image::OpticsProfileReceipt& receipt) {
    FfiOpticsReceipt result;
    result.status = optics_status(receipt.status);
    result.provider_id = rust::String(receipt.provider_id);
    result.provider_version = rust::String(receipt.provider_version);
    result.camera_profile = rust::String(receipt.camera_profile);
    result.lens_profile = rust::String(receipt.lens_profile);
    result.distortion_available = receipt.distortion_available;
    result.tca_available = receipt.tca_available;
    result.vignetting_available = receipt.vignetting_available;
    result.applied_distortion = receipt.applied_distortion;
    result.applied_tca = receipt.applied_tca;
    result.applied_vignetting = receipt.applied_vignetting;
    result.vignetting_used_distance_fallback = receipt.vignetting_used_distance_fallback;
    result.applied_scaling = receipt.applied_scaling;
    return result;
}

[[noreturn]] void throw_invalid_raw_development_plan(const std::string_view message) {
    throw image::DecodeError(
        image::DecodeErrorCode::invalid_request,
        0,
        std::string(message)
    );
}

// Private providers share a local ABI with the host, so a newer or malformed provider can still
// manufacture an enum discriminant that this host does not understand. Do not coerce that value
// into a benign-looking plan/receipt: doing so would corrupt source provenance and cache keys.
[[noreturn]] void throw_invalid_raw_development_provider_output(const std::string_view message) {
    throw image::DecodeError(
        image::DecodeErrorCode::unsupported,
        0,
        std::string(message)
    );
}

[[nodiscard]] FfiRawDevelopmentIntent raw_development_intent(
    const image::RawDevelopmentIntent value
) {
    switch (value) {
    case image::RawDevelopmentIntent::preview:
        return FfiRawDevelopmentIntent::Preview;
    case image::RawDevelopmentIntent::detail:
        return FfiRawDevelopmentIntent::Detail;
    case image::RawDevelopmentIntent::export_image:
        return FfiRawDevelopmentIntent::ExportImage;
    }
    throw_invalid_raw_development_provider_output(
        "RAW provider returned an unsupported development intent"
    );
}

[[nodiscard]] image::RawDevelopmentIntent raw_development_intent(
    const FfiRawDevelopmentIntent value
) {
    switch (value) {
    case FfiRawDevelopmentIntent::Preview:
        return image::RawDevelopmentIntent::preview;
    case FfiRawDevelopmentIntent::Detail:
        return image::RawDevelopmentIntent::detail;
    case FfiRawDevelopmentIntent::ExportImage:
        return image::RawDevelopmentIntent::export_image;
    }
    throw_invalid_raw_development_plan("RAW development intent is unsupported");
}

[[nodiscard]] FfiRawDevelopmentQuality raw_development_quality(
    const image::RawDevelopmentQuality value
) {
    switch (value) {
    case image::RawDevelopmentQuality::fast:
        return FfiRawDevelopmentQuality::Fast;
    case image::RawDevelopmentQuality::balanced:
        return FfiRawDevelopmentQuality::Balanced;
    case image::RawDevelopmentQuality::high:
        return FfiRawDevelopmentQuality::High;
    }
    throw_invalid_raw_development_provider_output(
        "RAW provider returned an unsupported development quality"
    );
}

[[nodiscard]] image::RawDevelopmentQuality raw_development_quality(
    const FfiRawDevelopmentQuality value
) {
    switch (value) {
    case FfiRawDevelopmentQuality::Fast:
        return image::RawDevelopmentQuality::fast;
    case FfiRawDevelopmentQuality::Balanced:
        return image::RawDevelopmentQuality::balanced;
    case FfiRawDevelopmentQuality::High:
        return image::RawDevelopmentQuality::high;
    }
    throw_invalid_raw_development_plan("RAW development quality is unsupported");
}

[[nodiscard]] FfiDngOpcodePolicy dng_opcode_policy(
    const image::DngOpcodePolicy value
) {
    switch (value) {
    case image::DngOpcodePolicy::provider_default:
        return FfiDngOpcodePolicy::ProviderDefault;
    case image::DngOpcodePolicy::require_applied:
        return FfiDngOpcodePolicy::RequireApplied;
    case image::DngOpcodePolicy::defer_to_shadow:
        return FfiDngOpcodePolicy::DeferToShadow;
    }
    throw_invalid_raw_development_provider_output(
        "RAW provider returned an unsupported DNG opcode policy"
    );
}

[[nodiscard]] image::DngOpcodePolicy dng_opcode_policy(const FfiDngOpcodePolicy value) {
    switch (value) {
    case FfiDngOpcodePolicy::ProviderDefault:
        return image::DngOpcodePolicy::provider_default;
    case FfiDngOpcodePolicy::RequireApplied:
        return image::DngOpcodePolicy::require_applied;
    case FfiDngOpcodePolicy::DeferToShadow:
        return image::DngOpcodePolicy::defer_to_shadow;
    }
    throw_invalid_raw_development_plan("DNG opcode policy is unsupported");
}

[[nodiscard]] FfiRawNoiseReductionIntent raw_noise_reduction_intent(
    const image::RawNoiseReductionIntent value
) {
    switch (value) {
    case image::RawNoiseReductionIntent::provider_default:
        return FfiRawNoiseReductionIntent::ProviderDefault;
    case image::RawNoiseReductionIntent::disabled:
        return FfiRawNoiseReductionIntent::Disabled;
    case image::RawNoiseReductionIntent::conservative:
        return FfiRawNoiseReductionIntent::Conservative;
    case image::RawNoiseReductionIntent::noise_robust:
        return FfiRawNoiseReductionIntent::NoiseRobust;
    }
    throw_invalid_raw_development_provider_output(
        "RAW provider returned an unsupported noise-reduction intent"
    );
}

[[nodiscard]] image::RawNoiseReductionIntent raw_noise_reduction_intent(
    const FfiRawNoiseReductionIntent value
) {
    switch (value) {
    case FfiRawNoiseReductionIntent::ProviderDefault:
        return image::RawNoiseReductionIntent::provider_default;
    case FfiRawNoiseReductionIntent::Disabled:
        return image::RawNoiseReductionIntent::disabled;
    case FfiRawNoiseReductionIntent::Conservative:
        return image::RawNoiseReductionIntent::conservative;
    case FfiRawNoiseReductionIntent::NoiseRobust:
        return image::RawNoiseReductionIntent::noise_robust;
    }
    throw_invalid_raw_development_plan("RAW noise-reduction intent is unsupported");
}

[[nodiscard]] FfiRawHighlightRecoveryIntent raw_highlight_recovery_intent(
    const image::RawHighlightRecoveryIntent value
) {
    switch (value) {
    case image::RawHighlightRecoveryIntent::provider_default:
        return FfiRawHighlightRecoveryIntent::ProviderDefault;
    case image::RawHighlightRecoveryIntent::disabled:
        return FfiRawHighlightRecoveryIntent::Disabled;
    case image::RawHighlightRecoveryIntent::conservative:
        return FfiRawHighlightRecoveryIntent::Conservative;
    case image::RawHighlightRecoveryIntent::aggressive:
        return FfiRawHighlightRecoveryIntent::Aggressive;
    }
    throw_invalid_raw_development_provider_output(
        "RAW provider returned an unsupported highlight-recovery intent"
    );
}

[[nodiscard]] image::RawHighlightRecoveryIntent raw_highlight_recovery_intent(
    const FfiRawHighlightRecoveryIntent value
) {
    switch (value) {
    case FfiRawHighlightRecoveryIntent::ProviderDefault:
        return image::RawHighlightRecoveryIntent::provider_default;
    case FfiRawHighlightRecoveryIntent::Disabled:
        return image::RawHighlightRecoveryIntent::disabled;
    case FfiRawHighlightRecoveryIntent::Conservative:
        return image::RawHighlightRecoveryIntent::conservative;
    case FfiRawHighlightRecoveryIntent::Aggressive:
        return image::RawHighlightRecoveryIntent::aggressive;
    }
    throw_invalid_raw_development_plan("RAW highlight-recovery intent is unsupported");
}

[[nodiscard]] FfiRawDevelopmentPlan raw_development_plan(
    const image::RawDevelopmentPlan& plan
) {
    return FfiRawDevelopmentPlan{
        plan.schema_version,
        raw_development_intent(plan.intent),
        raw_development_quality(plan.quality),
        dng_opcode_policy(plan.dng_opcode_policy),
        raw_noise_reduction_intent(plan.noise_reduction),
        raw_highlight_recovery_intent(plan.highlight_recovery),
    };
}

[[nodiscard]] image::RawDevelopmentPlan raw_development_plan(
    const FfiRawDevelopmentPlan& plan
) {
    return image::RawDevelopmentPlan{
        .schema_version = plan.schema_version,
        .intent = raw_development_intent(plan.intent),
        .quality = raw_development_quality(plan.quality),
        .dng_opcode_policy = dng_opcode_policy(plan.dng_opcode_policy),
        .noise_reduction = raw_noise_reduction_intent(plan.noise_reduction),
        .highlight_recovery = raw_highlight_recovery_intent(plan.highlight_recovery),
    };
}

[[nodiscard]] FfiRawDevelopmentCapabilities raw_development_capabilities(
    const image::RawDevelopmentCapabilities& capabilities
) noexcept {
    return FfiRawDevelopmentCapabilities{
        capabilities.schema_version,
        capabilities.available,
        capabilities.raw_frame,
        capabilities.dng_opcode_execution_receipt,
        capabilities.supported_intents,
        capabilities.supported_qualities,
        capabilities.supported_dng_opcode_policies,
        capabilities.supported_noise_reduction_intents,
        capabilities.supported_highlight_recovery_intents,
    };
}

[[nodiscard]] FfiRawDevelopmentPlanNegotiationStatus raw_development_plan_status(
    const image::RawDevelopmentPlanNegotiationStatus status
) {
    switch (status) {
    case image::RawDevelopmentPlanNegotiationStatus::accepted:
        return FfiRawDevelopmentPlanNegotiationStatus::Accepted;
    case image::RawDevelopmentPlanNegotiationStatus::adjusted:
        return FfiRawDevelopmentPlanNegotiationStatus::Adjusted;
    case image::RawDevelopmentPlanNegotiationStatus::rejected:
        return FfiRawDevelopmentPlanNegotiationStatus::Rejected;
    }
    throw_invalid_raw_development_provider_output(
        "RAW provider returned an unsupported development-plan negotiation status"
    );
}

[[nodiscard]] FfiRawDevelopmentPlanNegotiation raw_development_plan_negotiation(
    const image::RawDevelopmentPlanNegotiation& negotiation
) {
    return FfiRawDevelopmentPlanNegotiation{
        raw_development_plan(negotiation.requested),
        raw_development_plan(negotiation.effective),
        raw_development_plan_status(negotiation.status),
        static_cast<std::uint32_t>(negotiation.unresolved),
    };
}

[[nodiscard]] FfiDngOpcodeExecutionStatus dng_opcode_execution_status(
    const image::DngOpcodeExecutionStatus status
) {
    switch (status) {
    case image::DngOpcodeExecutionStatus::not_declared:
        return FfiDngOpcodeExecutionStatus::NotDeclared;
    case image::DngOpcodeExecutionStatus::provider_default:
        return FfiDngOpcodeExecutionStatus::ProviderDefault;
    case image::DngOpcodeExecutionStatus::applied:
        return FfiDngOpcodeExecutionStatus::Applied;
    case image::DngOpcodeExecutionStatus::deferred_to_shadow:
        return FfiDngOpcodeExecutionStatus::DeferredToShadow;
    case image::DngOpcodeExecutionStatus::skipped_for_preview:
        return FfiDngOpcodeExecutionStatus::SkippedForPreview;
    case image::DngOpcodeExecutionStatus::unsupported:
        return FfiDngOpcodeExecutionStatus::Unsupported;
    }
    throw_invalid_raw_development_provider_output(
        "RAW provider returned an unsupported DNG opcode execution status"
    );
}

[[nodiscard]] FfiRawDevelopmentReceipt raw_development_receipt(
    const image::RawDevelopmentReceipt& receipt
) {
    FfiRawDevelopmentReceipt result;
    result.schema_version = receipt.schema_version;
    result.provider_id = rust::String(receipt.provider_id);
    result.provider_version = rust::String(receipt.provider_version);
    result.library_version = rust::String(receipt.library_version);
    result.development_settings_signature = rust::String(receipt.development_settings_signature);
    result.requested_plan_identity = rust::String(receipt.requested_plan_identity);
    result.effective_plan_identity = rust::String(receipt.effective_plan_identity);
    result.requested_plan = raw_development_plan(receipt.requested_plan);
    result.effective_plan = raw_development_plan(receipt.effective_plan);
    result.plan_negotiation_status = raw_development_plan_status(
        receipt.plan_negotiation_status
    );
    result.processed_linear_reference_contract_version =
        receipt.processed_linear_reference_contract_version;
    result.declared_image_dimensions = dimensions(receipt.declared_image_dimensions);
    result.rendered_dimensions = dimensions(receipt.rendered_dimensions);
    result.orientation = receipt.orientation;
    result.half_size = receipt.half_size;
    result.use_camera_white_balance = receipt.use_camera_white_balance;
    result.use_camera_matrix = receipt.use_camera_matrix;
    result.use_auto_brightness = receipt.use_auto_brightness;
    result.use_exposure_correction = receipt.use_exposure_correction;
    result.brightness = receipt.brightness;
    result.maximum_adjustment_threshold = receipt.maximum_adjustment_threshold;
    result.output_bits_per_channel = receipt.output_bits_per_channel;
    result.demosaic_quality = receipt.demosaic_quality;
    result.output_color = receipt.output_color;
    result.gamma_inverse_power = receipt.gamma_inverse_power;
    result.gamma_linear_toe_slope = receipt.gamma_linear_toe_slope;
    result.dng_opcode_list_1_bytes = receipt.declared_dng_opcode_lists.dng_opcode_list_bytes[0];
    result.dng_opcode_list_2_bytes = receipt.declared_dng_opcode_lists.dng_opcode_list_bytes[1];
    result.dng_opcode_list_3_bytes = receipt.declared_dng_opcode_lists.dng_opcode_list_bytes[2];
    result.dng_opcode_list_1_execution = dng_opcode_execution_status(
        receipt.dng_opcode_execution[0]
    );
    result.dng_opcode_list_2_execution = dng_opcode_execution_status(
        receipt.dng_opcode_execution[1]
    );
    result.dng_opcode_list_3_execution = dng_opcode_execution_status(
        receipt.dng_opcode_execution[2]
    );
    result.process_warnings = receipt.process_warnings;
    return result;
}

[[nodiscard]] FfiRawPipelinePath raw_pipeline_path(const image::RawPipelinePath path) {
    switch (path) {
    case image::RawPipelinePath::decoded_raster:
        return FfiRawPipelinePath::DecodedRaster;
    case image::RawPipelinePath::shadow_raw_frame:
        return FfiRawPipelinePath::ShadowRawFrame;
    case image::RawPipelinePath::provider_processed_compatibility:
        return FfiRawPipelinePath::ProviderProcessedCompatibility;
    }
    throw_invalid_raw_development_provider_output(
        "RAW pipeline produced an unsupported source path"
    );
}

[[nodiscard]] FfiRawCameraProfileStatus raw_camera_profile_status(
    const image::RawCameraProfileStatus status
) {
    switch (status) {
    case image::RawCameraProfileStatus::not_considered:
        return FfiRawCameraProfileStatus::NotConsidered;
    case image::RawCameraProfileStatus::no_match:
        return FfiRawCameraProfileStatus::NoMatch;
    case image::RawCameraProfileStatus::applied:
        return FfiRawCameraProfileStatus::Applied;
    case image::RawCameraProfileStatus::matched_not_applied:
        return FfiRawCameraProfileStatus::MatchedNotApplied;
    }
    throw_invalid_raw_development_provider_output(
        "RAW pipeline produced an unsupported camera-profile status"
    );
}

[[nodiscard]] FfiRawPipelineReceipt raw_pipeline_receipt(
    const image::RawPipelineReceipt& receipt
) {
    if (receipt.schema_version != 0U && !receipt.valid()) {
        throw_invalid_raw_development_provider_output(
            "RAW pipeline produced an invalid route receipt"
        );
    }

    FfiRawPipelineReceipt result;
    result.schema_version = receipt.schema_version;
    result.path = raw_pipeline_path(receipt.path);
    result.cache_identity = receipt.schema_version == 0U
        ? rust::String() : rust::String(image::raw_pipeline_receipt_identity(receipt));
    result.pipeline_identity = rust::String(receipt.pipeline_identity);
    result.source_provider_id = rust::String(receipt.source_provider_id);
    result.source_provider_version = rust::String(receipt.source_provider_version);
    result.fallback_reason = rust::String(receipt.fallback_reason);
    result.raw_frame_schema_version = receipt.raw_frame_schema_version;
    result.raw_developer_version = receipt.raw_developer_version;
    result.requested_plan = raw_development_plan(receipt.requested_plan);
    result.effective_plan = raw_development_plan(receipt.effective_plan);
    result.camera_profile_status = raw_camera_profile_status(receipt.camera_profile_status);
    result.camera_profile_catalog_identity = rust::String(
        receipt.camera_profile_catalog_identity
    );
    result.camera_profile_identity = rust::String(receipt.camera_profile_identity);
    result.camera_profile_name = rust::String(receipt.camera_profile_name);
    result.camera_profile_diagnostic = rust::String(receipt.camera_profile_diagnostic);
    result.camera_profile_developer_version = receipt.camera_profile_developer_version;
    return result;
}

inline constexpr std::size_t maximum_adjustment_nodes = 256U;
inline constexpr std::size_t maximum_adjustment_node_id_bytes = 256U;

[[noreturn]] void throw_invalid_adjustment_plan(std::string message) {
    throw image::DecodeError(
        image::DecodeErrorCode::invalid_request,
        0,
        std::move(message)
    );
}

void require_parameter_count(
    const FfiAdjustmentNode& node,
    const std::size_t expected,
    const std::string_view operation
) {
    if (node.parameters.size() != expected) {
        throw_invalid_adjustment_plan(
            "adjustment node " + std::string(operation) + " requires exactly "
            + std::to_string(expected) + " parameters"
        );
    }
}

[[nodiscard]] image::AdjustmentNode adjustment_node(const FfiAdjustmentNode& source) {
    if (
        source.node_id.empty()
        || source.node_id.size() > maximum_adjustment_node_id_bytes
    ) {
        throw_invalid_adjustment_plan(
            "adjustment node id must contain between 1 and 256 UTF-8 bytes"
        );
    }

    image::AdjustmentNode result{
        .node_id = std::string(source.node_id.data(), source.node_id.size()),
        .parameter_schema_version = source.parameter_schema_version,
        .implementation_version = source.implementation_version,
        .enabled = source.enabled,
    };

    if (
        source.operation != FfiAdjustmentOperation::PerceptualColor
        && source.operation != FfiAdjustmentOperation::SpotHeal
        && !source.parameter_group_lengths.empty()
    ) {
        throw_invalid_adjustment_plan(
            "only operations with grouped parameter contracts accept group lengths"
        );
    }
    if (source.operation != FfiAdjustmentOperation::Lut3D && !source.payload.empty()) {
        throw_invalid_adjustment_plan(
            "only the 3D LUT operation accepts an immutable binary payload"
        );
    }

    switch (source.operation) {
    case FfiAdjustmentOperation::Exposure:
        require_parameter_count(source, 1U, "exposure");
        result.parameters = image::ExposureAdjustment{source.parameters[0]};
        break;
    case FfiAdjustmentOperation::Contrast:
        require_parameter_count(source, 2U, "contrast");
        result.parameters = image::ContrastAdjustment{
            source.parameters[0],
            source.parameters[1],
        };
        break;
    case FfiAdjustmentOperation::OklabLightnessToneCurve: {
        if (source.parameters.size() % 2U != 0U) {
            throw_invalid_adjustment_plan(
                "Oklab lightness curve parameters must contain flattened x/y pairs"
            );
        }
        const std::size_t point_count = source.parameters.size() / 2U;
        if (point_count < 2U || point_count > image::maximum_tone_curve_points) {
            throw_invalid_adjustment_plan(
                "Oklab lightness curve must contain between 2 and 256 control points"
            );
        }
        image::OklabLightnessToneCurve curve{
            .parameter_schema_version = source.parameter_schema_version,
            .implementation_version = source.implementation_version,
            .lightness = {},
        };
        // ToneCurveSet intentionally defaults to an identity pair for the native
        // authoring API. The FFI wire contract, however, carries the complete
        // point sequence. Clear that default before appending the transmitted
        // points; otherwise every non-identity curve becomes
        // (0,0) -> (1,1) -> authored points, which fails the strictly-increasing
        // x-coordinate validation and leaves the UI showing its stale preview.
        curve.lightness.points.clear();
        curve.lightness.points.reserve(point_count);
        for (std::size_t index = 0U; index < source.parameters.size(); index += 2U) {
            curve.lightness.points.push_back(image::ToneCurvePoint{
                source.parameters[index],
                source.parameters[index + 1U],
            });
        }
        result.parameters = std::move(curve);
        break;
    }
    case FfiAdjustmentOperation::RgbWhiteBalance:
        require_parameter_count(source, 2U, "RGB white balance");
        result.parameters = image::RgbWhiteBalanceAdjustment{
            .temperature = source.parameters[0],
            .tint = source.parameters[1],
        };
        break;
    case FfiAdjustmentOperation::Saturation:
        require_parameter_count(source, 1U, "saturation");
        result.parameters = image::SaturationAdjustment{source.parameters[0]};
        break;
    case FfiAdjustmentOperation::SelectiveTone:
        if (source.parameter_schema_version
                != image::selective_tone_v3_parameter_schema_version
            || source.implementation_version
                != image::selective_tone_v3_implementation_version) {
            throw_invalid_adjustment_plan(
                "selective tone requires the complete self-guided filter contract"
            );
        }
        require_parameter_count(source, 4U, "selective tone");
        result.parameters = image::SelectiveToneAdjustment{
            .highlights = source.parameters[0],
            .shadows = source.parameters[1],
            .whites = source.parameters[2],
            .blacks = source.parameters[3],
        };
        break;
    case FfiAdjustmentOperation::PerceptualColor: {
        if (source.parameter_schema_version
                != image::perceptual_color_v3_parameter_schema_version
            || source.implementation_version
                != image::perceptual_color_v3_implementation_version
            || source.parameter_group_lengths.size() != 1U) {
            throw_invalid_adjustment_plan(
                "perceptual color requires the current Color Mixer and Selective Color contract"
            );
        }
        const std::size_t additional_count = source.parameter_group_lengths[0];
        if (additional_count + 1U > image::maximum_point_color_ranges
            || source.parameters.size() != 70U + additional_count * 7U) {
            throw_invalid_adjustment_plan(
                "perceptual color has an invalid ordered range payload"
            );
        }
        if (source.parameters[25] != 0.0 && source.parameters[25] != 1.0) {
            throw_invalid_adjustment_plan(
                "perceptual color range enabled flag must be zero or one"
            );
        }
        image::PerceptualColorAdjustment parameters;
        parameters.vibrance = source.parameters[0];
        for (std::size_t index = 0; index < image::perceptual_hue_band_count; ++index) {
            parameters.hue[index] = source.parameters[1U + index];
            parameters.saturation[index] = source.parameters[9U + index];
            parameters.lightness[index] = source.parameters[17U + index];
        }
        parameters.color_range = image::PerceptualColorRange{
            .enabled = source.parameters[25] == 1.0,
            .center_degrees = source.parameters[26],
            .width_degrees = source.parameters[27],
            .softness = source.parameters[28],
            .hue_shift_degrees = source.parameters[29],
            .saturation = source.parameters[30],
            .lightness = source.parameters[31],
        };
        if (source.parameters[32] != 0.0 && source.parameters[32] != 1.0) {
            throw_invalid_adjustment_plan(
                "Selective Color relative flag must be zero or one"
            );
        }
        parameters.selective_color_relative = source.parameters[32] == 1.0;
        parameters.selective_color_lightness_protection = source.parameters[33];
        for (std::size_t target = 0U; target < image::selective_color_target_count; ++target) {
            for (std::size_t component = 0U;
                 component < image::selective_color_component_count;
                 ++component) {
                parameters.selective_color_cmyk[target][component] =
                    source.parameters[34U + target * image::selective_color_component_count
                                      + component];
            }
        }
        parameters.additional_color_ranges.reserve(additional_count);
        for (std::size_t range_index = 0U; range_index < additional_count; ++range_index) {
            const std::size_t offset = 70U + range_index * 7U;
            if (source.parameters[offset] != 0.0 && source.parameters[offset] != 1.0) {
                throw_invalid_adjustment_plan(
                    "perceptual color range enabled flag must be zero or one"
                );
            }
            parameters.additional_color_ranges.push_back(image::PerceptualColorRange{
                .enabled = source.parameters[offset] == 1.0,
                .center_degrees = source.parameters[offset + 1U],
                .width_degrees = source.parameters[offset + 2U],
                .softness = source.parameters[offset + 3U],
                .hue_shift_degrees = source.parameters[offset + 4U],
                .saturation = source.parameters[offset + 5U],
                .lightness = source.parameters[offset + 6U],
            });
        }
        result.parameters = parameters;
        break;
    }
    case FfiAdjustmentOperation::Lut3D: {
        require_parameter_count(source, 1U, "3D LUT");
        image::CubeLutAdjustment parameters{
            .lut = {},
            .intensity = source.parameters[0],
        };
        if (!source.payload.empty()) {
            parameters.lut = image::parse_cube_lut(std::string_view(
                reinterpret_cast<const char*>(source.payload.data()),
                source.payload.size()
            ));
        }
        result.parameters = std::move(parameters);
        break;
    }
    case FfiAdjustmentOperation::Sharpen: {
        if (source.parameter_schema_version
            != image::detail_effects_v3_parameter_schema_version) {
            throw_invalid_adjustment_plan(
                "detail and effects requires the current split-pass contract"
            );
        }
        image::DetailEffectsExecutionPass execution_pass;
        switch (source.implementation_version) {
        case image::technical_detail_v3_implementation_version:
            execution_pass = image::DetailEffectsExecutionPass::technical_detail;
            break;
        case image::color_grading_v3_implementation_version:
            execution_pass = image::DetailEffectsExecutionPass::color_grading;
            break;
        case image::finishing_effects_v3_implementation_version:
            execution_pass = image::DetailEffectsExecutionPass::finishing_effects;
            break;
        default:
            throw_invalid_adjustment_plan(
                "detail and effects execution pass is unsupported"
            );
        }
        require_parameter_count(source, 35U, "detail and effects");
        image::SharpenAdjustment parameters{
            .execution_pass = execution_pass,
            .amount = source.parameters[0],
            .radius = source.parameters[1],
            .threshold = source.parameters[2],
            .masking = source.parameters[3],
        };
        {
            parameters.clarity = source.parameters[4];
            parameters.texture = source.parameters[5];
            parameters.denoise_luminance = source.parameters[6];
            parameters.denoise_detail = source.parameters[7];
            parameters.denoise_color = source.parameters[8];
            parameters.dehaze = source.parameters[9];
            parameters.defringe_purple_amount = source.parameters[10];
            parameters.defringe_purple_hue_low = source.parameters[11];
            parameters.defringe_purple_hue_high = source.parameters[12];
            parameters.defringe_green_amount = source.parameters[13];
            parameters.defringe_green_hue_low = source.parameters[14];
            parameters.defringe_green_hue_high = source.parameters[15];
            parameters.shadows_hue = source.parameters[16];
            parameters.shadows_saturation = source.parameters[17];
            parameters.shadows_luminance = source.parameters[18];
            parameters.midtones_hue = source.parameters[19];
            parameters.midtones_saturation = source.parameters[20];
            parameters.midtones_luminance = source.parameters[21];
            parameters.highlights_hue = source.parameters[22];
            parameters.highlights_saturation = source.parameters[23];
            parameters.highlights_luminance = source.parameters[24];
            parameters.grading_blending = source.parameters[25];
            parameters.grading_balance = source.parameters[26];
            parameters.grain_amount = source.parameters[27];
            parameters.grain_size = source.parameters[28];
            parameters.grain_roughness = source.parameters[29];
            parameters.vignette_amount = source.parameters[30];
            parameters.vignette_midpoint = source.parameters[31];
            parameters.vignette_roundness = source.parameters[32];
            parameters.vignette_feather = source.parameters[33];
            parameters.vignette_highlights = source.parameters[34];
        }
        result.parameters = parameters;
        break;
    }
    case FfiAdjustmentOperation::SpotHeal: {
        if (source.parameter_group_lengths.size() != 1U) {
            throw_invalid_adjustment_plan(
                "spot-heal requires one target-count parameter group"
            );
        }
        const std::size_t target_count = source.parameter_group_lengths[0];
        if (target_count == 0U || target_count > 64U
            || source.parameters.size() != target_count * 3U) {
            throw_invalid_adjustment_plan(
                "spot-heal must contain 1 through 64 flattened x/y/radius targets"
            );
        }
        image::SpotHealAdjustment parameters;
        parameters.spots.reserve(target_count);
        for (std::size_t index = 0U; index < target_count; ++index) {
            const std::size_t offset = index * 3U;
            const double encoded_radius = source.parameters[offset + 2U];
            if (!std::isfinite(source.parameters[offset])
                || !std::isfinite(source.parameters[offset + 1U])
                || !std::isfinite(encoded_radius)
                || source.parameters[offset] < 0.0 || source.parameters[offset] > 1.0
                || source.parameters[offset + 1U] < 0.0
                || source.parameters[offset + 1U] > 1.0
                || encoded_radius < 1.0 || encoded_radius > 128.0
                || std::floor(encoded_radius) != encoded_radius) {
                throw_invalid_adjustment_plan(
                    "spot-heal target coordinates or radius are outside the supported range"
                );
            }
            parameters.spots.push_back(image::SpotHealTarget{
                .center_x = source.parameters[offset],
                .center_y = source.parameters[offset + 1U],
                .radius_level_zero_pixels = static_cast<std::uint16_t>(encoded_radius),
            });
        }
        result.parameters = std::move(parameters);
        break;
    }
    default:
        throw_invalid_adjustment_plan("adjustment node operation is unsupported");
    }

    return result;
}

[[nodiscard]] std::vector<image::AdjustmentNode> adjustment_nodes(
    const rust::Vec<FfiAdjustmentNode>& nodes
) {
    if (nodes.empty() || nodes.size() > maximum_adjustment_nodes) {
        throw_invalid_adjustment_plan(
            "adjustment render plan must contain between 1 and 256 nodes"
        );
    }

    std::vector<image::AdjustmentNode> result;
    result.reserve(nodes.size());
    for (const auto& source : nodes) {
        result.push_back(adjustment_node(source));
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<image::AdjustmentLayer>> adjustment_layers(
    const rust::Vec<FfiAdjustmentNode>& source
) {
    const bool has_boundaries = std::any_of(
        source.begin(),
        source.end(),
        [](const FfiAdjustmentNode& node) {
            return node.operation == FfiAdjustmentOperation::LocalMaskLayerStart
                || node.operation == FfiAdjustmentOperation::LocalMaskLayerEnd;
        }
    );
    if (!has_boundaries) {
        return std::nullopt;
    }
    if (source.empty() || source.size() > maximum_adjustment_nodes) {
        throw_invalid_adjustment_plan(
            "local-mask adjustment stream must contain between 1 and 256 nodes"
        );
    }

    std::vector<image::AdjustmentLayer> layers;
    std::optional<image::AdjustmentLayer> open_layer;
    for (const auto& node : source) {
        if (node.operation == FfiAdjustmentOperation::LocalMaskLayerStart) {
            if (open_layer.has_value()) {
                throw_invalid_adjustment_plan("local-mask layers may not nest");
            }
            if (node.parameter_schema_version != image::adjustment_parameter_schema_version
                || node.implementation_version != image::adjustment_implementation_version
                || !node.payload.empty() || !node.parameter_group_lengths.empty()
                || node.parameters.size() != 10U) {
                throw_invalid_adjustment_plan("local-mask layer start has an invalid contract");
            }
            for (const double value : node.parameters) {
                if (!std::isfinite(value)) {
                    throw_invalid_adjustment_plan("local-mask layer start has a non-finite parameter");
                }
            }
            const double opacity = node.parameters[0];
            const double kind = node.parameters[1];
            const double invert = node.parameters[9];
            if (opacity < 0.0 || opacity > 1.0
                || (kind != 0.0 && kind != 1.0 && kind != 2.0)
                || (invert != 0.0 && invert != 1.0)) {
                throw_invalid_adjustment_plan("local-mask layer start has an out-of-range parameter");
            }
            image::AdjustmentLayer layer{
                .layer_id = std::string(node.node_id.data(), node.node_id.size()),
                .enabled = node.enabled,
                .opacity = opacity,
                .mask = std::nullopt,
                .nodes = {},
            };
            if (kind == 1.0) {
                layer.mask = image::LocalMask{
                    .kind = image::LocalMaskKind::linear_gradient,
                    .x0 = node.parameters[2],
                    .y0 = node.parameters[3],
                    .x1 = node.parameters[4],
                    .y1 = node.parameters[5],
                    .invert = invert == 1.0,
                };
            } else if (kind == 2.0) {
                layer.mask = image::LocalMask{
                    .kind = image::LocalMaskKind::radial_gradient,
                    .x0 = node.parameters[2],
                    .y0 = node.parameters[3],
                    .radius_x = node.parameters[6],
                    .radius_y = node.parameters[7],
                    .feather = node.parameters[8],
                    .invert = invert == 1.0,
                };
            }
            open_layer = std::move(layer);
            continue;
        }
        if (node.operation == FfiAdjustmentOperation::LocalMaskLayerEnd) {
            if (!open_layer.has_value() || !node.parameters.empty() || !node.payload.empty()
                || !node.parameter_group_lengths.empty()) {
                throw_invalid_adjustment_plan("local-mask layer end has no matching valid start");
            }
            if (open_layer->nodes.empty()) {
                throw_invalid_adjustment_plan("local-mask layer must contain at least one adjustment");
            }
            layers.push_back(std::move(*open_layer));
            open_layer.reset();
            continue;
        }
        if (!open_layer.has_value()) {
            throw_invalid_adjustment_plan(
                "local-mask adjustment appears outside a complete layer boundary"
            );
        }
        open_layer->nodes.push_back(adjustment_node(node));
    }
    // Sixteen user Grade Nodes plus one photo-local repair layer. The latter
    // is compiler-owned and never appears as a second user node limit.
    if (open_layer.has_value() || layers.empty() || layers.size() > 17U) {
        throw_invalid_adjustment_plan("local-mask layer stream is incomplete or exceeds 17 layers");
    }
    return layers;
}

[[nodiscard]] std::filesystem::path filesystem_path_from_utf8(const rust::Str path) {
    const std::string_view utf8_bytes(path.data(), path.size());
    std::u8string utf8_path;
    utf8_path.reserve(utf8_bytes.size());
    for (const char byte : utf8_bytes) {
        utf8_path.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path(utf8_path);
}

[[nodiscard]] std::unique_ptr<DecodeHandle> open_provider_path(
    std::unique_ptr<image::DecoderProvider> provider,
    const std::filesystem::path& path
) {
    auto session = provider->open(path);
    return std::make_unique<DecodeHandle>(
        std::move(provider),
        std::move(session),
        image::make_lensfun_optics_provider()
    );
}

[[nodiscard]] rust::Vec<FfiOpticsProfileCandidate> optics_profile_candidates_for(
    const image::DecodeSession& session
) {
    // JPEG/HEIF input may already have vendor lens corrections baked in. The first raster
    // implementation therefore keeps automatic optics discovery RAW-only; `raw_count` describes
    // the source format without coupling profile discovery to whether this provider can unpack
    // its sensor pixels (for example Nikon HE/HE*).
    const auto& metadata = session.metadata();
    if (metadata.raw_count == 0U) {
        return {};
    }
    const auto provider = image::make_lensfun_optics_provider();
    const auto candidates = provider->profile_candidates(metadata);
    rust::Vec<FfiOpticsProfileCandidate> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        FfiOpticsProfileCandidate ffi;
        ffi.camera_maker = rust::String(candidate.camera_maker);
        ffi.camera_model = rust::String(candidate.camera_model);
        ffi.lens_maker = rust::String(candidate.lens_maker);
        ffi.lens_model = rust::String(candidate.lens_model);
        result.push_back(std::move(ffi));
    }
    return result;
}

[[nodiscard]] image::AssetMetadata asset_metadata(const FfiMetadataSnapshot& source) {
    image::AssetMetadata metadata;
    metadata.make = std::string(source.make);
    metadata.model = std::string(source.model);
    metadata.normalized_make = std::string(source.normalized_make);
    metadata.normalized_model = std::string(source.normalized_model);
    metadata.dng_version = std::string(source.dng_version);
    metadata.raw_count = source.raw_count;
    metadata.raw_dimensions = image::Dimensions{
        source.raw_dimensions.width,
        source.raw_dimensions.height,
    };
    metadata.image_dimensions = image::Dimensions{
        source.image_dimensions.width,
        source.image_dimensions.height,
    };
    metadata.margins = image::Margins{
        source.margins.left,
        source.margins.top,
        source.margins.right,
        source.margins.bottom,
    };
    metadata.orientation = source.orientation;
    metadata.cfa_pattern = std::string(source.cfa_pattern);
    metadata.sensor_colors = source.sensor_colors;
    metadata.sensor_bits = source.sensor_bits;
    metadata.black_level = source.black_level;
    metadata.white_level = source.white_level;
    metadata.as_shot_neutral = {
        source.as_shot_neutral_r,
        source.as_shot_neutral_g1,
        source.as_shot_neutral_b,
        source.as_shot_neutral_g2,
    };
    metadata.baseline_exposure = source.baseline_exposure;
    metadata.iso_speed = source.iso_speed;
    metadata.exposure_time_seconds = source.exposure_time_seconds;
    metadata.aperture_f_number = source.aperture_f_number;
    metadata.focal_length_mm = source.focal_length_mm;
    metadata.captured_at_unix_seconds = source.captured_at_unix_seconds;
    metadata.lens_make = std::string(source.lens_make);
    metadata.lens_model = std::string(source.lens_model);
    metadata.focal_length_35mm = source.focal_length_35mm;
    return metadata;
}

} // namespace

DecodeHandle::DecodeHandle(
    std::unique_ptr<image::DecoderProvider> provider,
    std::unique_ptr<image::DecodeSession> session,
    std::shared_ptr<const image::OpticsProvider> optics_provider
)
    : provider_(std::move(provider)), session_(std::move(session)),
      optics_provider_(std::move(optics_provider)) {}

DecodeHandle::~DecodeHandle() = default;

void DecodeHandle::configure_optics(const FfiOpticsSettings& settings) {
    image::OpticsSettings configured{
        settings.schema_version,
        settings.enabled,
        settings.correct_distortion,
        settings.correct_tca,
        settings.correct_vignetting,
        settings.automatic_scale,
        settings.manual_distortion,
        settings.manual_tca_red_cyan,
        settings.manual_tca_blue_yellow,
        settings.manual_vignetting_amount,
        settings.manual_vignetting_midpoint,
        std::string(settings.camera_profile_maker),
        std::string(settings.camera_profile_model),
        std::string(settings.lens_profile_maker),
        std::string(settings.lens_profile_model),
    };
    // The signature function is the authoritative schema/combination validator
    // shared with cache identity construction.
    (void)image::optics_settings_signature(configured);
    optics_settings_ = configured;
}

FfiProviderSnapshot DecodeHandle::provider() const {
    const auto& info = provider_->info();
    FfiProviderSnapshot snapshot;
    snapshot.id = rust::String(info.id);
    snapshot.version = rust::String(info.version);
    snapshot.dng_sdk = info.dng_sdk;
    snapshot.rawspeed = info.rawspeed;
    snapshot.jpeg = info.jpeg;
    return snapshot;
}

FfiMetadataSnapshot DecodeHandle::metadata() const {
    const auto& metadata = session_->metadata();
    FfiMetadataSnapshot snapshot;
    snapshot.make = rust::String(metadata.make);
    snapshot.model = rust::String(metadata.model);
    snapshot.normalized_make = rust::String(metadata.normalized_make);
    snapshot.normalized_model = rust::String(metadata.normalized_model);
    snapshot.dng_version = rust::String(metadata.dng_version);
    snapshot.raw_count = metadata.raw_count;
    snapshot.raw_dimensions = dimensions(metadata.raw_dimensions);
    snapshot.image_dimensions = dimensions(metadata.image_dimensions);
    snapshot.margins = FfiMargins{
        metadata.margins.left,
        metadata.margins.top,
        metadata.margins.right,
        metadata.margins.bottom,
    };
    snapshot.orientation = metadata.orientation;
    snapshot.cfa_pattern = rust::String(metadata.cfa_pattern);
    snapshot.sensor_colors = metadata.sensor_colors;
    snapshot.sensor_bits = metadata.sensor_bits;
    snapshot.black_level = metadata.black_level;
    snapshot.white_level = metadata.white_level;
    snapshot.as_shot_neutral_r = metadata.as_shot_neutral[0];
    snapshot.as_shot_neutral_g1 = metadata.as_shot_neutral[1];
    snapshot.as_shot_neutral_b = metadata.as_shot_neutral[2];
    snapshot.as_shot_neutral_g2 = metadata.as_shot_neutral[3];
    snapshot.baseline_exposure = metadata.baseline_exposure;
    snapshot.iso_speed = metadata.iso_speed;
    snapshot.exposure_time_seconds = metadata.exposure_time_seconds;
    snapshot.aperture_f_number = metadata.aperture_f_number;
    snapshot.focal_length_mm = metadata.focal_length_mm;
    snapshot.captured_at_unix_seconds = metadata.captured_at_unix_seconds;
    snapshot.lens_make = rust::String(metadata.lens_make);
    snapshot.lens_model = rust::String(metadata.lens_model);
    snapshot.focal_length_35mm = metadata.focal_length_35mm;
    return snapshot;
}

FfiCapabilitySnapshot DecodeHandle::capabilities() const {
    const auto& capabilities = session_->capabilities();
    const auto& opcode_bytes = capabilities.pending_corrections.dng_opcode_list_bytes;
    FfiCapabilitySnapshot snapshot;
    snapshot.metadata = capabilities.metadata;
    snapshot.embedded_previews = capabilities.embedded_previews;
    snapshot.raw_frame = capabilities.raw_frame;
    snapshot.reference_rgb = capabilities.reference_rgb;
    snapshot.dng_opcode_list_1_bytes = opcode_bytes[0];
    snapshot.dng_opcode_list_2_bytes = opcode_bytes[1];
    snapshot.dng_opcode_list_3_bytes = opcode_bytes[2];
    snapshot.raw_development = shadow::bridge::raw_development_capabilities(
        capabilities.raw_development
    );
    return snapshot;
}

FfiRawDevelopmentCapabilities DecodeHandle::raw_development_capabilities() const {
    return shadow::bridge::raw_development_capabilities(
        session_->raw_development_capabilities()
    );
}

FfiRawDevelopmentPlanNegotiation DecodeHandle::negotiate_raw_development_plan(
    const FfiRawDevelopmentPlan& plan
) const {
    return raw_development_plan_negotiation(
        session_->negotiate_raw_development_plan(raw_development_plan(plan))
    );
}

FfiRawDevelopmentReceipt DecodeHandle::raw_development_receipt() const {
    return shadow::bridge::raw_development_receipt(raw_development_receipt_);
}

FfiRawPipelineReceipt DecodeHandle::raw_pipeline_receipt() const {
    return shadow::bridge::raw_pipeline_receipt(raw_pipeline_receipt_);
}

rust::Vec<FfiPreviewSnapshot> DecodeHandle::previews() const {
    rust::Vec<FfiPreviewSnapshot> snapshots;
    snapshots.reserve(session_->previews().size());
    for (const auto& preview : session_->previews()) {
        snapshots.push_back(preview_snapshot(preview));
    }
    return snapshots;
}

FfiPreviewPayload DecodeHandle::decode_best_preview() {
    const auto selected = image::select_best_preview(session_->previews());
    FfiPreviewPayload result;
    if (!selected.has_value()) {
        result.present = false;
        return result;
    }

    auto payload = session_->decode_preview(*selected);
    result.present = true;
    result.descriptor = preview_snapshot(payload.descriptor);
    result.byte_order = byte_order(payload.byte_order);
    result.bytes.reserve(payload.bytes.size());
    for (const auto byte : payload.bytes) {
        result.bytes.push_back(byte);
    }
    return result;
}

FfiEncodedProxy DecodeHandle::render_reference_proxy(
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality
) const {
    const auto proxy = image::render_reference_proxy_jpeg(
        *session_,
        image::ProxyRequest{max_edge, jpeg_quality}
    );
    return encoded_proxy(proxy);
}

FfiEncodedProxy DecodeHandle::render_adjustment_plan(
    const FfiAdjustmentRenderRequest& request
) const {
    const auto layers = adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    const auto preview = image::prepare_warm_edit_preview(
        *session_,
        request.max_edge,
        optics_provider_.get(),
        optics_settings_
    );
    const auto proxy = layers.has_value()
        ? preview.render_jpeg_layers(*layers, request.jpeg_quality, geometry)
        : preview.render_jpeg(adjustment_nodes(request.nodes), request.jpeg_quality, geometry);
    return encoded_proxy(proxy);
}

FfiSensorClippingMask DecodeHandle::sensor_clipping_mask(
    const std::uint32_t target_width,
    const std::uint32_t target_height
) const {
    if (target_width == 0U || target_height == 0U) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "sensor clipping mask dimensions must be non-zero"
        );
    }
    if (!session_->capabilities().raw_frame) {
        return {};
    }
    return ffi_sensor_clipping_mask(image::project_sensor_clipping_mask(
        session_->decode_raw_frame(),
        image::Dimensions{target_width, target_height}
    ));
}

std::unique_ptr<EditPreviewHandle> DecodeHandle::prepare_edit_preview(
    const std::uint32_t max_edge
) const {
    auto prepared = image::prepare_warm_edit_preview(
        *session_,
        max_edge,
        optics_provider_.get(),
        optics_settings_
    );
    raw_development_receipt_ = prepared.raw_development_receipt();
    raw_pipeline_receipt_ = prepared.raw_pipeline_receipt();
    return std::make_unique<EditPreviewHandle>(std::move(prepared));
}

std::unique_ptr<EditPreviewHandle> DecodeHandle::prepare_edit_preview_with_raw_development_plan(
    const std::uint32_t max_edge,
    const FfiRawDevelopmentPlan& plan
) const {
    auto prepared = image::prepare_warm_edit_preview(
        *session_,
        max_edge,
        raw_development_plan(plan),
        optics_provider_.get(),
        optics_settings_
    );
    raw_development_receipt_ = prepared.raw_development_receipt();
    raw_pipeline_receipt_ = prepared.raw_pipeline_receipt();
    return std::make_unique<EditPreviewHandle>(std::move(prepared));
}

EditPreviewHandle::EditPreviewHandle(image::WarmEditPreviewSession session)
    : session_(std::move(session)) {}

EditPreviewHandle::~EditPreviewHandle() = default;

FfiDimensions EditPreviewHandle::dimensions() const noexcept {
    return shadow::bridge::dimensions(session_.dimensions());
}

std::uint32_t EditPreviewHandle::max_edge() const noexcept {
    return session_.max_edge();
}

FfiOpticsReceipt EditPreviewHandle::optics_receipt() const {
    return shadow::bridge::optics_receipt(session_.optics_receipt());
}

FfiRawDevelopmentReceipt EditPreviewHandle::raw_development_receipt() const {
    return shadow::bridge::raw_development_receipt(session_.raw_development_receipt());
}

FfiRawPipelineReceipt EditPreviewHandle::raw_pipeline_receipt() const {
    return shadow::bridge::raw_pipeline_receipt(session_.raw_pipeline_receipt());
}

bool EditPreviewCancellationHandle::cancel() const noexcept {
    return source_.request_stop();
}

std::stop_token EditPreviewCancellationHandle::token() const noexcept {
    return source_.get_token();
}

std::shared_ptr<EditPreviewCancellationHandle> new_edit_preview_cancellation() {
    return std::make_shared<EditPreviewCancellationHandle>();
}

FfiEncodedProxy EditPreviewHandle::render_adjustment_plan(
    const FfiAdjustmentRenderRequest& request
) const {
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    return encoded_proxy(
        layers.has_value()
            ? session_.render_jpeg_layers(*layers, request.jpeg_quality, geometry)
            : session_.render_jpeg(adjustment_nodes(request.nodes), request.jpeg_quality, geometry)
    );
}

FfiAnalyzedEditPreview EditPreviewHandle::render_adjustment_plan_with_analysis(
    const FfiAdjustmentRenderRequest& request
) const {
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    return analyzed_edit_preview(
        layers.has_value()
            ? session_.render_jpeg_with_analysis_layers(*layers, request.jpeg_quality, geometry)
            : session_.render_jpeg_with_analysis(
                  adjustment_nodes(request.nodes),
                  request.jpeg_quality,
                  geometry
              )
    );
}

FfiCancellableEncodedProxy EditPreviewHandle::render_adjustment_plan_cancellable(
    const FfiAdjustmentRenderRequest& request,
    const EditPreviewCancellationHandle& cancellation
) const {
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    if (layers.has_value()) {
        if (cancellation.token().stop_requested()) {
            return FfiCancellableEncodedProxy{.cancelled = true, .proxy = {}};
        }
        auto rendered = session_.render_jpeg_layers(*layers, request.jpeg_quality, geometry);
        return FfiCancellableEncodedProxy{
            .cancelled = cancellation.token().stop_requested(),
            .proxy = cancellation.token().stop_requested() ? FfiEncodedProxy{} : encoded_proxy(rendered),
        };
    }
    const auto nodes = adjustment_nodes(request.nodes);
    auto rendered = session_.render_jpeg_cancellable(
        nodes,
        request.jpeg_quality,
        cancellation.token(),
        geometry
    );
    if (rendered.cancelled()) {
        return FfiCancellableEncodedProxy{
            .cancelled = true,
            .proxy = {},
        };
    }
    return FfiCancellableEncodedProxy{
        .cancelled = false,
        .proxy = encoded_proxy(*rendered.completed),
    };
}

FfiCancellableAnalyzedEditPreview
EditPreviewHandle::render_adjustment_plan_with_analysis_cancellable(
    const FfiAdjustmentRenderRequest& request,
    const EditPreviewCancellationHandle& cancellation
) const {
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    if (layers.has_value()) {
        if (cancellation.token().stop_requested()) {
            return FfiCancellableAnalyzedEditPreview{.cancelled = true, .preview = {}};
        }
        auto rendered = session_.render_jpeg_with_analysis_layers(
            *layers,
            request.jpeg_quality,
            geometry
        );
        return FfiCancellableAnalyzedEditPreview{
            .cancelled = cancellation.token().stop_requested(),
            .preview = cancellation.token().stop_requested()
                ? FfiAnalyzedEditPreview{}
                : analyzed_edit_preview(rendered),
        };
    }
    const auto nodes = adjustment_nodes(request.nodes);
    auto rendered = session_.render_jpeg_with_analysis_cancellable(
        nodes,
        request.jpeg_quality,
        cancellation.token(),
        geometry
    );
    if (rendered.cancelled()) {
        return FfiCancellableAnalyzedEditPreview{
            .cancelled = true,
            .preview = {},
        };
    }
    return FfiCancellableAnalyzedEditPreview{
        .cancelled = false,
        .preview = analyzed_edit_preview(*rendered.completed),
    };
}

std::unique_ptr<FullEditDetailHandle> DecodeHandle::prepare_edit_detail() const {
    auto prepared = image::prepare_full_edit_detail(
        *session_,
        optics_provider_.get(),
        optics_settings_
    );
    raw_development_receipt_ = prepared.raw_development_receipt();
    raw_pipeline_receipt_ = prepared.raw_pipeline_receipt();
    return std::make_unique<FullEditDetailHandle>(std::move(prepared));
}

std::unique_ptr<FullEditDetailHandle> DecodeHandle::prepare_edit_detail_with_raw_development_plan(
    const FfiRawDevelopmentPlan& plan
) const {
    auto prepared = image::prepare_full_edit_detail(
        *session_,
        raw_development_plan(plan),
        optics_provider_.get(),
        optics_settings_
    );
    raw_development_receipt_ = prepared.raw_development_receipt();
    raw_pipeline_receipt_ = prepared.raw_pipeline_receipt();
    return std::make_unique<FullEditDetailHandle>(std::move(prepared));
}

FullEditDetailHandle::FullEditDetailHandle(image::FullEditDetailSession session)
    : session_(std::move(session)) {}

FullEditDetailHandle::~FullEditDetailHandle() = default;

FfiDimensions FullEditDetailHandle::dimensions() const noexcept {
    return shadow::bridge::dimensions(session_.dimensions());
}

std::uint64_t FullEditDetailHandle::retained_bytes() const noexcept {
    return session_.retained_bytes();
}

FfiOpticsReceipt FullEditDetailHandle::optics_receipt() const {
    return shadow::bridge::optics_receipt(session_.optics_receipt());
}

FfiRawDevelopmentReceipt FullEditDetailHandle::raw_development_receipt() const {
    return shadow::bridge::raw_development_receipt(session_.raw_development_receipt());
}

FfiRawPipelineReceipt FullEditDetailHandle::raw_pipeline_receipt() const {
    return shadow::bridge::raw_pipeline_receipt(session_.raw_pipeline_receipt());
}

FfiRenderedDetailTile FullEditDetailHandle::render_adjustment_plan_tile(
    const FfiAdjustmentDetailTileRequest& request
) const {
    const auto layers = adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    return rendered_detail_tile(
        layers.has_value()
            ? session_.render_rgb8_layers(*layers, detail_tile_rect(request.rect), geometry)
            : session_.render_rgb8(
                  adjustment_nodes(request.nodes),
                  detail_tile_rect(request.rect),
                  geometry
              )
    );
}

std::unique_ptr<DecodeHandle> open_libraw_utf8(const rust::Str path) {
    auto provider = image::make_libraw_decoder_provider();
    return open_provider_path(std::move(provider), filesystem_path_from_utf8(path));
}

std::unique_ptr<DecodeHandle> open_photo_utf8(const rust::Str path) {
    auto provider = image::make_photo_decoder_provider();
    return open_provider_path(std::move(provider), filesystem_path_from_utf8(path));
}

rust::Vec<FfiOpticsProfileCandidate> query_libraw_optics_profiles_utf8(const rust::Str path) {
    auto decoder = image::make_libraw_decoder_provider();
    auto session = decoder->open(filesystem_path_from_utf8(path));
    return optics_profile_candidates_for(*session);
}

rust::Vec<FfiOpticsProfileCandidate> query_photo_optics_profiles_utf8(const rust::Str path) {
    auto decoder = image::make_photo_decoder_provider();
    auto session = decoder->open(filesystem_path_from_utf8(path));
    return optics_profile_candidates_for(*session);
}

rust::Vec<FfiOpticsProfileCandidate> query_optics_profiles_for_metadata(
    const FfiMetadataSnapshot& source
) {
    const auto provider = image::make_lensfun_optics_provider();
    const auto candidates = provider->profile_candidates(asset_metadata(source));
    rust::Vec<FfiOpticsProfileCandidate> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        FfiOpticsProfileCandidate ffi;
        ffi.camera_maker = rust::String(candidate.camera_maker);
        ffi.camera_model = rust::String(candidate.camera_model);
        ffi.lens_maker = rust::String(candidate.lens_maker);
        ffi.lens_model = rust::String(candidate.lens_model);
        result.push_back(std::move(ffi));
    }
    return result;
}

rust::String libraw_provider_version() {
    const auto provider = image::make_libraw_decoder_provider();
    return rust::String(
        provider->info().version + ";source-render="
        + std::to_string(image::source_rendering_implementation_version)
        + ";source-profiles=" + image::load_local_source_profile_catalog().identity
    );
}

rust::String photo_provider_version() {
    const auto provider = image::make_photo_decoder_provider();
    return rust::String(provider->info().version);
}

rust::String edit_preview_generator_implementation_identity() {
    return rust::String(image::edit_preview_generator_implementation_identity());
}

rust::Vec<rust::String> photo_supported_raster_extensions() {
    rust::Vec<rust::String> result;
    const auto extensions = image::raster_supported_file_extensions();
    result.reserve(extensions.size());
    for (const auto& extension : extensions) {
        result.emplace_back(extension);
    }
    return result;
}

rust::String raw_development_plan_identity(const FfiRawDevelopmentPlan& plan) {
    const std::string identity = image::raw_development_plan_identity(
        raw_development_plan(plan)
    );
    return rust::String(identity);
}

FfiEncodedProxy render_photo_reference_proxy(
    const rust::Str path,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality
) {
    auto provider = image::make_photo_decoder_provider();
    auto session = provider->open(filesystem_path_from_utf8(path));
    return encoded_proxy(image::render_reference_proxy_jpeg(
        *session,
        image::ProxyRequest{max_edge, jpeg_quality}
    ));
}

FfiDisplayLuma decode_jpeg_display_luma(
    const rust::Slice<const std::uint8_t> encoded,
    const std::uint32_t max_edge
) {
    const auto decoded = image::decode_jpeg_display_luma(
        std::span<const std::uint8_t>(encoded.data(), encoded.size()),
        max_edge
    );
    if (decoded.row_stride_samples > std::numeric_limits<std::uint32_t>::max()) {
        throw image::DecodeError(
            image::DecodeErrorCode::internal,
            0,
            "JPEG display-luma stride does not fit the bridge contract"
        );
    }

    FfiDisplayLuma result;
    result.width = decoded.dimensions.width;
    result.height = decoded.dimensions.height;
    result.stride = static_cast<std::uint32_t>(decoded.row_stride_samples);
    result.preprocessing_version = rust::String(decoded.preprocessing_version);
    result.samples.reserve(decoded.samples.size());
    for (const float sample : decoded.samples) {
        result.samples.push_back(sample);
    }
    return result;
}

} // namespace shadow::bridge
