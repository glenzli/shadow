#include <shadow/image/cxx_bridge.hpp>

#include "adjustment_render_wire.hpp"
#include "cxx_bridge_projection.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/display_luma.hpp>
#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_white_balance.hpp>
#include <shadow/image/sensor_clipping.hpp>
#include <shadow/image/source_profile_catalog.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::bridge {

namespace cxx_bridge_projection {

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

[[nodiscard]] FfiSensorClippingMask sensor_clipping_mask(const image::SensorClippingMask& mask) {
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
[[nodiscard]] rust::Vec<std::uint64_t>
sample_counts(const std::array<std::uint64_t, Size>& source) {
    rust::Vec<std::uint64_t> result;
    result.reserve(source.size());
    for (const auto count : source) {
        result.push_back(count);
    }
    return result;
}

[[nodiscard]] FfiEditPreviewAnalysis
edit_preview_analysis(const image::EditPreviewAnalysis& analysis) {
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
    result.hdr_headroom_bins = sample_counts(analysis.hdr_headroom_bins);
    result.hdr_headroom_pixels = analysis.hdr_headroom_pixels;
    result.hdr_peak_headroom_ev = analysis.hdr_peak_headroom_ev;
    result.pixel_count = analysis.pixel_count;
    result.shadow_clipped_pixels = analysis.shadow_clipped_pixels;
    result.highlight_clipped_pixels = analysis.highlight_clipped_pixels;
    return result;
}

[[nodiscard]] FfiEditPreviewBackend edit_preview_backend(const image::EditPreviewBackend backend) {
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

[[nodiscard]] FfiEditPreviewExecutionReceipt
edit_preview_execution_receipt(const image::EditPreviewExecutionReceipt& receipt) {
    if (!receipt.valid()) {
        throw image::DecodeError(
            image::DecodeErrorCode::internal,
            0,
            "edit-preview execution receipt is invalid"
        );
    }
    FfiEditPreviewExecutionReceipt result;
    result.schema_version = receipt.schema_version;
    result.cache_identity = rust::String(image::edit_preview_execution_receipt_identity(receipt));
    result.adjustment_backend = edit_preview_backend(receipt.adjustment_backend);
    result.adjustment_backend_version = receipt.adjustment_backend_version;
    result.adjustment_execution_contract_version = receipt.adjustment_execution_contract_version;
    result.display_backend = edit_preview_backend(receipt.display_backend);
    result.display_backend_version = receipt.display_backend_version;
    result.display_output_contract_version = receipt.display_output_contract_version;
    result.fused_pipeline = receipt.fused_pipeline;
    result.adjustment_fell_back = receipt.adjustment_fell_back;
    result.display_fell_back = receipt.display_fell_back;
    result.diagnostic = rust::String(receipt.diagnostic);
    return result;
}

[[nodiscard]] FfiAnalyzedEditPreview
analyzed_edit_preview(const image::AnalyzedEditPreview& preview) {
    FfiAnalyzedEditPreview result;
    result.proxy = encoded_proxy(preview.proxy);
    result.analysis = edit_preview_analysis(preview.analysis);
    result.execution = edit_preview_execution_receipt(preview.execution);
    return result;
}

[[nodiscard]] FfiEditPreviewMaskCoverage
edit_preview_mask_coverage(const std::optional<image::EditPreviewMaskCoverage>& coverage) {
    FfiEditPreviewMaskCoverage result{};
    if (!coverage.has_value()) {
        return result;
    }
    if (!coverage->valid()) {
        throw image::DecodeError(
            image::DecodeErrorCode::internal,
            0,
            "completed edit-preview mask coverage violates its native contract"
        );
    }
    result.available = true;
    result.version = rust::String(coverage->version);
    result.layer_index = coverage->layer_index;
    result.dimensions = dimensions(coverage->dimensions);
    result.row_stride_bytes = coverage->row_stride_bytes;
    result.samples.reserve(coverage->samples.size());
    for (const auto sample : coverage->samples) {
        result.samples.push_back(sample);
    }
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
        .straighten_degrees = value.straighten_degrees,
        .perspective_vertical = value.perspective_vertical,
        .perspective_horizontal = value.perspective_horizontal,
        .flip_horizontal = value.flip_horizontal,
        .flip_vertical = value.flip_vertical,
    };
    image::validate_photo_geometry(geometry);
    return geometry;
}

[[nodiscard]] FfiRenderedDetailTile rendered_detail_tile(const image::RenderedDetailTile& tile) {
    FfiRenderedDetailTile result;
    result.rect = detail_tile_rect(tile.rect);
    result.full_dimensions = dimensions(tile.full_dimensions);
    result.row_stride_bytes = tile.row_stride_bytes;
    result.bytes.reserve(tile.bytes.size());
    for (const auto byte : tile.bytes) {
        result.bytes.push_back(byte);
    }
    switch (tile.execution.backend) {
    case image::DetailTileRenderBackend::cpu:
        result.execution_backend = 0U;
        break;
    case image::DetailTileRenderBackend::metal:
        result.execution_backend = 1U;
        break;
    }
    result.execution_backend_version = tile.execution.backend_version;
    result.source_cache_hit = tile.execution.source_cache_hit;
    result.fell_back = tile.execution.fell_back;
    result.diagnostic = rust::String(tile.execution.diagnostic);
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
    throw image::DecodeError(image::DecodeErrorCode::invalid_request, 0, std::string(message));
}

// Private providers share a local ABI with the host, so a newer or malformed provider can still
// manufacture an enum discriminant that this host does not understand. Do not coerce that value
// into a benign-looking plan/receipt: doing so would corrupt source provenance and cache keys.
[[noreturn]] void throw_invalid_raw_development_provider_output(const std::string_view message) {
    throw image::DecodeError(image::DecodeErrorCode::unsupported, 0, std::string(message));
}

[[nodiscard]] FfiRawDevelopmentIntent
raw_development_intent(const image::RawDevelopmentIntent value) {
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

[[nodiscard]] image::RawDevelopmentIntent
raw_development_intent(const FfiRawDevelopmentIntent value) {
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

[[nodiscard]] FfiRawDevelopmentQuality
raw_development_quality(const image::RawDevelopmentQuality value) {
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

[[nodiscard]] image::RawDevelopmentQuality
raw_development_quality(const FfiRawDevelopmentQuality value) {
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

[[nodiscard]] FfiDngOpcodePolicy dng_opcode_policy(const image::DngOpcodePolicy value) {
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

[[nodiscard]] FfiRawNoiseReductionIntent
raw_noise_reduction_intent(const image::RawNoiseReductionIntent value) {
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

[[nodiscard]] image::RawNoiseReductionIntent
raw_noise_reduction_intent(const FfiRawNoiseReductionIntent value) {
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

[[nodiscard]] FfiRawHighlightRecoveryIntent
raw_highlight_recovery_intent(const image::RawHighlightRecoveryIntent value) {
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

[[nodiscard]] image::RawHighlightRecoveryIntent
raw_highlight_recovery_intent(const FfiRawHighlightRecoveryIntent value) {
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

[[nodiscard]] FfiRawWhiteBalanceMode
raw_white_balance_mode(const image::RawWhiteBalanceMode value) {
    switch (value) {
    case image::RawWhiteBalanceMode::as_shot:
        return FfiRawWhiteBalanceMode::AsShot;
    case image::RawWhiteBalanceMode::temperature_tint:
        return FfiRawWhiteBalanceMode::TemperatureTint;
    }
    throw_invalid_raw_development_provider_output(
        "RAW provider returned an unsupported white-balance mode"
    );
}

[[nodiscard]] image::RawWhiteBalanceMode
raw_white_balance_mode(const FfiRawWhiteBalanceMode value) {
    switch (value) {
    case FfiRawWhiteBalanceMode::AsShot:
        return image::RawWhiteBalanceMode::as_shot;
    case FfiRawWhiteBalanceMode::TemperatureTint:
        return image::RawWhiteBalanceMode::temperature_tint;
    }
    throw_invalid_raw_development_plan("RAW white-balance mode is unsupported");
}

[[nodiscard]] FfiRawDevelopmentPlan raw_development_plan(const image::RawDevelopmentPlan& plan) {
    return FfiRawDevelopmentPlan{
        plan.schema_version,
        raw_development_intent(plan.intent),
        raw_development_quality(plan.quality),
        dng_opcode_policy(plan.dng_opcode_policy),
        raw_noise_reduction_intent(plan.noise_reduction),
        raw_highlight_recovery_intent(plan.highlight_recovery),
        raw_white_balance_mode(plan.white_balance.mode),
        plan.white_balance.temperature_kelvin,
        plan.white_balance.tint,
    };
}

[[nodiscard]] image::RawDevelopmentPlan raw_development_plan(const FfiRawDevelopmentPlan& plan) {
    return image::RawDevelopmentPlan{
        .schema_version = plan.schema_version,
        .intent = raw_development_intent(plan.intent),
        .quality = raw_development_quality(plan.quality),
        .dng_opcode_policy = dng_opcode_policy(plan.dng_opcode_policy),
        .noise_reduction = raw_noise_reduction_intent(plan.noise_reduction),
        .highlight_recovery = raw_highlight_recovery_intent(plan.highlight_recovery),
        .white_balance = image::RawWhiteBalance{
            .mode = raw_white_balance_mode(plan.white_balance_mode),
            .temperature_kelvin = plan.temperature_kelvin,
            .tint = plan.tint,
        },
    };
}

[[nodiscard]] FfiRawDevelopmentCapabilities
raw_development_capabilities(const image::RawDevelopmentCapabilities& capabilities) noexcept {
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

[[nodiscard]] FfiRawDevelopmentPlanNegotiationStatus
raw_development_plan_status(const image::RawDevelopmentPlanNegotiationStatus status) {
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

[[nodiscard]] FfiRawDevelopmentPlanNegotiation
raw_development_plan_negotiation(const image::RawDevelopmentPlanNegotiation& negotiation) {
    return FfiRawDevelopmentPlanNegotiation{
        raw_development_plan(negotiation.requested),
        raw_development_plan(negotiation.effective),
        raw_development_plan_status(negotiation.status),
        static_cast<std::uint32_t>(negotiation.unresolved),
    };
}

[[nodiscard]] FfiDngOpcodeExecutionStatus
dng_opcode_execution_status(const image::DngOpcodeExecutionStatus status) {
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

[[nodiscard]] FfiRawDevelopmentReceipt
raw_development_receipt(const image::RawDevelopmentReceipt& receipt) {
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
    result.plan_negotiation_status = raw_development_plan_status(receipt.plan_negotiation_status);
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
    result.dng_opcode_list_1_execution =
        dng_opcode_execution_status(receipt.dng_opcode_execution[0]);
    result.dng_opcode_list_2_execution =
        dng_opcode_execution_status(receipt.dng_opcode_execution[1]);
    result.dng_opcode_list_3_execution =
        dng_opcode_execution_status(receipt.dng_opcode_execution[2]);
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

[[nodiscard]] FfiRawCameraProfileStatus
raw_camera_profile_status(const image::RawCameraProfileStatus status) {
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

[[nodiscard]] FfiRawPipelineReceipt raw_pipeline_receipt(const image::RawPipelineReceipt& receipt) {
    if (receipt.schema_version != 0U && !receipt.valid()) {
        throw_invalid_raw_development_provider_output(
            "RAW pipeline produced an invalid route receipt"
        );
    }

    FfiRawPipelineReceipt result;
    result.schema_version = receipt.schema_version;
    result.path = raw_pipeline_path(receipt.path);
    result.cache_identity = receipt.schema_version == 0U
                                ? rust::String()
                                : rust::String(image::raw_pipeline_receipt_identity(receipt));
    result.pipeline_identity = rust::String(receipt.pipeline_identity);
    result.source_provider_id = rust::String(receipt.source_provider_id);
    result.source_provider_version = rust::String(receipt.source_provider_version);
    result.fallback_reason = rust::String(receipt.fallback_reason);
    result.raw_frame_schema_version = receipt.raw_frame_schema_version;
    result.raw_developer_version = receipt.raw_developer_version;
    result.requested_plan = raw_development_plan(receipt.requested_plan);
    result.effective_plan = raw_development_plan(receipt.effective_plan);
    result.camera_profile_status = raw_camera_profile_status(receipt.camera_profile_status);
    result.camera_profile_catalog_identity = rust::String(receipt.camera_profile_catalog_identity);
    result.camera_profile_identity = rust::String(receipt.camera_profile_identity);
    result.camera_profile_name = rust::String(receipt.camera_profile_name);
    result.camera_profile_diagnostic = rust::String(receipt.camera_profile_diagnostic);
    result.camera_profile_developer_version = receipt.camera_profile_developer_version;
    return result;
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

[[nodiscard]] rust::Vec<FfiOpticsProfileCandidate>
optics_profile_candidates_for(const image::DecodeSession& session) {
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
    if (source.has_focus_observation) {
        const auto focus_source = source.focus_observation_source == 1U
            ? image::FocusObservationSource::camera_focus_area
            : source.focus_observation_source == 2U
            ? image::FocusObservationSource::camera_focus_location
            : image::FocusObservationSource::unknown;
        if (focus_source != image::FocusObservationSource::unknown) {
            metadata.focus_observation = image::FocusObservation{
                .schema_version = source.focus_observation_schema_version,
                .source = focus_source,
                .center_x = source.focus_observation_center_x,
                .center_y = source.focus_observation_center_y,
                .width = source.focus_observation_width,
                .height = source.focus_observation_height,
                .focus_confirmed = source.focus_observation_confirmed,
                .confidence = source.focus_observation_confidence,
            };
        }
    }
    metadata.captured_at_unix_seconds = source.captured_at_unix_seconds;
    metadata.has_gps_coordinates = source.has_gps_coordinates;
    metadata.gps_latitude_degrees = source.gps_latitude_degrees;
    metadata.gps_longitude_degrees = source.gps_longitude_degrees;
    metadata.has_gps_altitude = source.has_gps_altitude;
    metadata.gps_altitude_meters = source.gps_altitude_meters;
    metadata.lens_make = std::string(source.lens_make);
    metadata.lens_model = std::string(source.lens_model);
    metadata.focal_length_35mm = source.focal_length_35mm;
    return metadata;
}

} // namespace cxx_bridge_projection

using namespace cxx_bridge_projection;

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

rust::Vec<FfiOpticsProfileCandidate>
query_optics_profiles_for_metadata(const FfiMetadataSnapshot& source) {
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

FfiRawWhiteBalancePresentation
query_raw_white_balance_presentation_for_metadata(const FfiMetadataSnapshot& source) {
    FfiRawWhiteBalancePresentation result{
        .available = false,
        .temperature_kelvin = 5'500U,
        .tint = 0,
    };
    const image::AssetMetadata metadata = asset_metadata(source);
    const auto* const definition =
        image::match_camera_profile(image::default_camera_profile_catalog(), metadata);
    if (definition == nullptr || metadata.cfa_pattern.size() != 4U) {
        return result;
    }

    image::RawFrameDescriptor descriptor;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.cfa_pattern = metadata.cfa_pattern;
    descriptor.as_shot_neutral = metadata.as_shot_neutral;
    for (std::size_t site = 0U; site < descriptor.bayer_2x2.size(); ++site) {
        switch (metadata.cfa_pattern[site]) {
        case 'R':
        case 'r':
            descriptor.bayer_2x2[site] = image::RawCfaColor::red;
            break;
        case 'G':
        case 'g':
            descriptor.bayer_2x2[site] = image::RawCfaColor::green;
            break;
        case 'B':
        case 'b':
            descriptor.bayer_2x2[site] = image::RawCfaColor::blue;
            break;
        default:
            return result;
        }
    }
    const auto neutral = image::raw_as_shot_camera_neutral(descriptor);
    if (!neutral.has_value()) {
        return result;
    }
    const auto presentation =
        image::raw_dcp_white_balance_presentation(definition->profile, *neutral);
    if (!presentation.has_value()) {
        return result;
    }
    const auto temperature = std::llround(presentation->temperature_kelvin);
    const auto tint = std::llround(presentation->tint);
    if (temperature < 2'000LL || temperature > 25'000LL
        || tint < -150LL || tint > 150LL) {
        return result;
    }
    result.available = true;
    result.temperature_kelvin = static_cast<std::uint32_t>(temperature);
    result.tint = static_cast<std::int16_t>(tint);
    return result;
}

rust::String libraw_provider_version() {
    const auto provider = image::make_libraw_decoder_provider();
    return rust::String(
        provider->info().version
        + ";source-render=" + std::to_string(image::source_rendering_implementation_version)
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
    const std::string identity = image::raw_development_plan_identity(raw_development_plan(plan));
    return rust::String(identity);
}

FfiEncodedProxy render_photo_reference_proxy(
    const rust::Str path,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality
) {
    auto provider = image::make_photo_decoder_provider();
    auto session = provider->open(filesystem_path_from_utf8(path));
    return encoded_proxy(
        image::render_reference_proxy_jpeg(*session, image::ProxyRequest{max_edge, jpeg_quality})
    );
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
