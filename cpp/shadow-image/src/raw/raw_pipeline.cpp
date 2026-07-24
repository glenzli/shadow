#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/fused_raw_development.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image {

namespace {

inline constexpr std::string_view raw_pipeline_environment = "SHADOW_RAW_PIPELINE";
inline constexpr std::string_view raw_frame_pipeline_identity =
    "shadow-raw-frame-developer-v1:bayer-area-preview+bayer-bilinear:"
    "as-shot-neutral:camera-matrix:linear-srgb-u16";

[[nodiscard]] const char* path_name(const RawPipelinePath path) noexcept {
    switch (path) {
    case RawPipelinePath::decoded_raster:
        return "decoded-raster";
    case RawPipelinePath::shadow_raw_frame:
        return "shadow-raw-frame";
    case RawPipelinePath::provider_processed_compatibility:
        return "provider-processed-compatibility";
    }
    return "unknown";
}

[[nodiscard]] const char* mode_name(const RawPipelineMode mode) noexcept {
    switch (mode) {
    case RawPipelineMode::automatic:
        return "auto";
    case RawPipelineMode::require_shadow_raw_frame:
        return "raw-frame";
    case RawPipelineMode::require_provider_processed:
        return "processed";
    }
    return "unknown";
}

[[nodiscard]] const char* camera_profile_status_name(
    const RawCameraProfileStatus status
) noexcept {
    switch (status) {
    case RawCameraProfileStatus::not_considered:
        return "not-considered";
    case RawCameraProfileStatus::no_match:
        return "no-match";
    case RawCameraProfileStatus::applied:
        return "applied";
    case RawCameraProfileStatus::matched_not_applied:
        return "matched-not-applied";
    }
    return "unknown";
}

[[nodiscard]] bool supported_orientation(const std::int32_t orientation) noexcept {
    return orientation == 0 || orientation == 3 || orientation == 5 || orientation == 6;
}

[[nodiscard]] Dimensions oriented_dimensions(
    const Dimensions dimensions,
    const std::int32_t orientation
) noexcept {
    if (orientation == 5 || orientation == 6) {
        return Dimensions{dimensions.height, dimensions.width};
    }
    return dimensions;
}

[[nodiscard]] std::array<double, 3U> canonical_camera_neutral(
    const RawFrameDescriptor& descriptor
) {
    std::array<double, 3U> totals{};
    std::array<std::uint32_t, 3U> counts{};
    for (std::size_t site = 0U; site < descriptor.bayer_2x2.size(); ++site) {
        std::size_t channel = 0U;
        switch (descriptor.bayer_2x2[site]) {
        case RawCfaColor::red:
            channel = 0U;
            break;
        case RawCfaColor::green:
            channel = 1U;
            break;
        case RawCfaColor::blue:
            channel = 2U;
            break;
        case RawCfaColor::unknown:
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "RAW frame has an unknown CFA colour in its neutral calibration"
            );
        }
        const double neutral = descriptor.as_shot_neutral[site];
        if (!std::isfinite(neutral) || neutral <= 0.0) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "RAW frame does not provide a usable as-shot camera neutral"
            );
        }
        totals[channel] += neutral;
        ++counts[channel];
    }
    std::array<double, 3U> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel) {
        if (counts[channel] == 0U) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                0,
                "RAW frame camera neutral does not cover RGB"
            );
        }
        result[channel] = totals[channel] / static_cast<double>(counts[channel]);
    }
    return result;
}

[[nodiscard]] std::array<double, 3U> white_balance_multipliers(
    const RawFrameDescriptor& descriptor
) {
    const auto neutral = canonical_camera_neutral(descriptor);
    std::array<double, 3U> multipliers{
        1.0 / neutral[0],
        1.0 / neutral[1],
        1.0 / neutral[2],
    };
    const double green = multipliers[1];
    if (!std::isfinite(green) || green <= 0.0) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "RAW frame camera neutral produces an invalid white balance"
        );
    }
    for (auto& value : multipliers) {
        value /= green;
    }
    return multipliers;
}

using Matrix3 = std::array<double, 9U>;

[[nodiscard]] Matrix3 multiply_matrix(
    const Matrix3& left,
    const Matrix3& right
) noexcept {
    Matrix3 result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            for (std::size_t inner = 0U; inner < 3U; ++inner) {
                result[row * 3U + column] +=
                    left[row * 3U + inner] * right[inner * 3U + column];
            }
        }
    }
    return result;
}

[[nodiscard]] RawFrameLinearTransform generic_raw_frame_transform(
    const RawFrameDescriptor& descriptor
) {
    Matrix3 camera_to_srgb{};
    if (descriptor.has_camera_to_linear_srgb_d65) {
        camera_to_srgb = descriptor.camera_to_linear_srgb_d65;
    } else {
        if (!descriptor.has_camera_to_xyz_d50) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "RAW frame does not provide a camera colour calibration"
            );
        }
        // RawFrame's D50 matrix is input-major for compatibility with the original provider ABI:
        // XYZ[j] = sum(camera[i] * matrix[i * 3 + j]). Transpose it once into the
        // output-major convention consumed by the fused renderer.
        Matrix3 camera_to_xyz_d50{};
        for (std::size_t input = 0U; input < 3U; ++input) {
            for (std::size_t output = 0U; output < 3U; ++output) {
                camera_to_xyz_d50[output * 3U + input] =
                    descriptor.camera_to_xyz_d50[input * 3U + output];
            }
        }
        constexpr Matrix3 d50_to_d65{
            0.9555766, -0.0230393, 0.0631636,
            -0.0282895, 1.0099416, 0.0210077,
            0.0122982, -0.0204830, 1.3299098,
        };
        constexpr Matrix3 xyz_d65_to_srgb{
            3.2404542, -1.5371385, -0.4985314,
            -0.9692660, 1.8760108, 0.0415560,
            0.0556434, -0.2040259, 1.0572252,
        };
        camera_to_srgb = multiply_matrix(
            xyz_d65_to_srgb,
            multiply_matrix(d50_to_d65, camera_to_xyz_d50)
        );
    }

    const auto white_balance = white_balance_multipliers(descriptor);
    // Fold WB into the input columns so the hot loop performs one matrix multiply.
    for (std::size_t output = 0U; output < 3U; ++output) {
        for (std::size_t input = 0U; input < 3U; ++input) {
            camera_to_srgb[output * 3U + input] *= white_balance[input];
        }
    }
    return RawFrameLinearTransform{camera_to_srgb};
}

[[nodiscard]] RawDevelopmentReceipt raw_frame_development_receipt(
    const RawFrameDescriptor& descriptor,
    const RawDevelopmentPlan& plan,
    const Dimensions rendered_dimensions,
    const RawDemosaicReceipt& demosaic,
    const RawDevelopmentBackend backend,
    const DcpColorTransform* camera_profile
) {
    RawDevelopmentReceipt receipt;
    receipt.schema_version = raw_development_receipt_schema_version;
    receipt.provider_id = descriptor.provider_id.empty()
        ? "provider-neutral-raw-frame" : descriptor.provider_id;
    receipt.provider_version = descriptor.provider_version.empty()
        ? "unrecorded" : descriptor.provider_version;
    receipt.development_settings_signature =
        demosaic.algorithm == RawDemosaicAlgorithm::bayer_area_preview_v1
        ? "shadow-raw-v1;demosaic=bayer-area-preview"
        : "shadow-raw-v1;demosaic=bayer-bilinear";
    receipt.development_settings_signature += ";backend="
        + std::string(raw_development_backend_identity(backend));
    if (camera_profile != nullptr) {
        receipt.development_settings_signature += ";color=dcp;"
            + dcp_color_receipt_identity(camera_profile->receipt);
    } else {
        receipt.development_settings_signature += ";wb=as-shot;matrix=provider-generic";
    }
    receipt.requested_plan_identity = raw_development_plan_identity(plan);
    receipt.effective_plan_identity = receipt.requested_plan_identity;
    receipt.requested_plan = plan;
    receipt.effective_plan = plan;
    receipt.plan_negotiation_status = RawDevelopmentPlanNegotiationStatus::accepted;
    receipt.processed_linear_reference_contract_version =
        processed_linear_reference_rgb_contract_version;
    receipt.declared_image_dimensions = descriptor.active_dimensions;
    receipt.rendered_dimensions = rendered_dimensions;
    receipt.orientation = descriptor.orientation;
    receipt.half_size = rendered_dimensions != oriented_dimensions(
        descriptor.active_dimensions,
        descriptor.orientation
    );
    receipt.use_camera_white_balance = true;
    receipt.use_camera_matrix = true;
    receipt.use_auto_brightness = false;
    receipt.use_exposure_correction = false;
    receipt.brightness = 1.0F;
    receipt.maximum_adjustment_threshold = 0.0F;
    receipt.output_bits_per_channel = 16U;
    receipt.demosaic_quality = demosaic.algorithm
        == RawDemosaicAlgorithm::bayer_area_preview_v1 ? 1 : 3;
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

struct DevelopedRawFrame final {
    PixelBuffer pixels;
    RawDevelopmentBackend backend = RawDevelopmentBackend::cpu;
};

[[nodiscard]] DevelopedRawFrame develop_raw_frame(
    RawFrame frame,
    const RawDevelopmentPlan& plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const DcpColorTransform* camera_profile
) {
    if (!frame.valid() || !frame.is_bayer_2x2()) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "Shadow's RAW developer currently requires a valid Bayer two-by-two frame"
        );
    }
    if (!supported_orientation(frame.descriptor.orientation)) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "Shadow's RAW developer does not support this source orientation"
        );
    }
    if (frame.descriptor.declared_pending_corrections.has_pending()) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Shadow's RAW developer cannot yet execute this source's declared DNG opcodes"
        );
    }
    if (
        camera_profile == nullptr
        && !frame.descriptor.has_camera_to_linear_srgb_d65
        && !frame.descriptor.has_camera_to_xyz_d50
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Shadow's RAW developer requires an explicit camera colour transform"
        );
    }

    const RawFrameLinearTransform transform = camera_profile == nullptr
        ? generic_raw_frame_transform(frame.descriptor)
        : RawFrameLinearTransform{camera_profile->camera_to_linear_srgb_d65};
    FusedRawFrameDevelopment developed = develop_bayer_linear_srgb_u16_fused(
        frame,
        transform,
        preview_max_edge
    );
    PixelBuffer output = std::move(developed.pixels);
    output.raw_development_receipt = raw_frame_development_receipt(
        frame.descriptor,
        plan,
        output.dimensions,
        developed.demosaic_receipt,
        developed.backend,
        camera_profile
    );
    return DevelopedRawFrame{
        .pixels = std::move(output),
        .backend = developed.backend,
    };
}

[[nodiscard]] DevelopedSourceReference provider_processed_source(
    const DecodeSession& session,
    const RawDevelopmentPlan& plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawPipelinePath path,
    std::string fallback_reason
) {
    PixelBuffer pixels = preview_max_edge.has_value()
        ? session.render_reference_rgb_for_preview(*preview_max_edge, plan)
        : session.render_reference_rgb(plan);
    RawPipelineReceipt pipeline;
    pipeline.path = path;
    pipeline.pipeline_identity = path == RawPipelinePath::decoded_raster
        ? "shadow-decoded-raster-v1" : "shadow-provider-processed-compatibility-v1";
    pipeline.source_provider_id = pixels.raw_development_receipt.provider_id;
    pipeline.source_provider_version = pixels.raw_development_receipt.provider_version;
    pipeline.fallback_reason = std::move(fallback_reason);
    pipeline.requested_plan = plan;
    pipeline.effective_plan = pixels.raw_development_receipt.recorded()
        ? pixels.raw_development_receipt.effective_plan : plan;
    if (!pipeline.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "provider processed source produced an invalid RAW pipeline receipt"
        );
    }
    return DevelopedSourceReference{
        .pixels = std::move(pixels),
        .pipeline_receipt = std::move(pipeline),
    };
}

[[nodiscard]] bool can_fallback_from(const DecodeError& error) noexcept {
    return error.code() == DecodeErrorCode::unsupported
        || error.code() == DecodeErrorCode::unsupported_layout;
}

[[nodiscard]] RawDevelopmentPlanNegotiation negotiate_shadow_raw_frame_plan(
    const RawDevelopmentPlan& requested
) noexcept {
    RawDevelopmentCapabilities capabilities;
    capabilities.available = true;
    capabilities.raw_frame = true;
    capabilities.supported_intents =
        raw_development_intent_mask(RawDevelopmentIntent::preview)
        | raw_development_intent_mask(RawDevelopmentIntent::detail)
        | raw_development_intent_mask(RawDevelopmentIntent::export_image);
    capabilities.supported_qualities =
        raw_development_quality_mask(RawDevelopmentQuality::balanced);
    capabilities.supported_dng_opcode_policies =
        dng_opcode_policy_mask(DngOpcodePolicy::provider_default);
    capabilities.supported_noise_reduction_intents =
        raw_noise_reduction_intent_mask(RawNoiseReductionIntent::provider_default)
        | raw_noise_reduction_intent_mask(RawNoiseReductionIntent::disabled);
    capabilities.supported_highlight_recovery_intents =
        raw_highlight_recovery_intent_mask(RawHighlightRecoveryIntent::provider_default)
        | raw_highlight_recovery_intent_mask(RawHighlightRecoveryIntent::disabled);

    auto negotiation = shadow::image::negotiate_raw_development_plan(
        requested,
        capabilities
    );
    // The first owned developer has one full-quality bilinear/area baseline. A caller asking for
    // the future high-quality tier can still proceed, but the downgrade must be visible in both
    // source and pipeline receipts so it cannot alias a later high-quality implementation.
    if (
        !negotiation.accepted()
        && negotiation.unresolved == RawDevelopmentPlanAspect::quality
        && requested.quality == RawDevelopmentQuality::high
    ) {
        auto effective = requested;
        effective.quality = RawDevelopmentQuality::balanced;
        if (capabilities.supports(effective)) {
            negotiation.effective = effective;
            negotiation.status = RawDevelopmentPlanNegotiationStatus::adjusted;
            negotiation.unresolved = RawDevelopmentPlanAspect::none;
        }
    }
    return negotiation;
}

} // namespace

bool RawPipelineReceipt::valid() const noexcept {
    if (
        schema_version != raw_pipeline_receipt_schema_version || pipeline_identity.empty()
        || requested_plan.schema_version != raw_development_plan_schema_version
        || effective_plan.schema_version != raw_development_plan_schema_version
    ) {
        return false;
    }
    if (path == RawPipelinePath::shadow_raw_frame) {
        const bool raw_frame_is_valid =
            raw_frame_schema_version == shadow::image::raw_frame_schema_version
            && raw_developer_version == shadow_raw_frame_developer_version
            && fallback_reason.empty();
        if (!raw_frame_is_valid) {
            return false;
        }
        switch (camera_profile_status) {
        case RawCameraProfileStatus::not_considered:
            return false;
        case RawCameraProfileStatus::no_match:
            return !camera_profile_catalog_identity.empty()
                && camera_profile_identity.empty() && camera_profile_name.empty()
                && camera_profile_diagnostic.empty()
                && camera_profile_developer_version == dcp_color_developer_version;
        case RawCameraProfileStatus::applied:
            return !camera_profile_catalog_identity.empty()
                && !camera_profile_identity.empty() && !camera_profile_name.empty()
                && camera_profile_diagnostic.empty()
                && camera_profile_developer_version == dcp_color_developer_version;
        case RawCameraProfileStatus::matched_not_applied:
            return !camera_profile_catalog_identity.empty()
                && !camera_profile_identity.empty() && !camera_profile_name.empty()
                && !camera_profile_diagnostic.empty()
                && camera_profile_developer_version == dcp_color_developer_version;
        }
        return false;
    }
    return raw_frame_schema_version == 0U && raw_developer_version == 0U
        && camera_profile_status == RawCameraProfileStatus::not_considered
        && camera_profile_catalog_identity.empty() && camera_profile_identity.empty()
        && camera_profile_name.empty() && camera_profile_diagnostic.empty()
        && camera_profile_developer_version == 0U;
}

RawPipelinePolicy raw_pipeline_policy_from_environment() {
    const auto* configured = std::getenv(raw_pipeline_environment.data());
    if (configured == nullptr || *configured == '\0' || std::string_view(configured) == "auto") {
        return default_raw_pipeline_policy();
    }
    if (std::string_view(configured) == "raw-frame") {
        return RawPipelinePolicy{
            .schema_version = raw_pipeline_policy_schema_version,
            .mode = RawPipelineMode::require_shadow_raw_frame,
        };
    }
    if (std::string_view(configured) == "processed") {
        return RawPipelinePolicy{
            .schema_version = raw_pipeline_policy_schema_version,
            .mode = RawPipelineMode::require_provider_processed,
        };
    }
    throw DecodeError(
        DecodeErrorCode::invalid_request,
        0,
        "SHADOW_RAW_PIPELINE must be auto, raw-frame, or processed"
    );
}

std::string raw_pipeline_policy_identity(const RawPipelinePolicy& policy) {
    if (policy.schema_version != raw_pipeline_policy_schema_version) {
        throw std::invalid_argument("RAW pipeline policy schema is unsupported");
    }
    return "raw-pipeline-policy-v1;mode=" + std::string(mode_name(policy.mode));
}

std::string raw_pipeline_receipt_identity(const RawPipelineReceipt& receipt) {
    if (!receipt.valid()) {
        throw std::invalid_argument("RAW pipeline receipt is invalid");
    }
    std::ostringstream identity;
    identity << "raw-pipeline-receipt-v1;path=" << path_name(receipt.path)
             << ";pipeline=" << receipt.pipeline_identity
             << ";provider=" << receipt.source_provider_id
             << ";provider-version=" << receipt.source_provider_version
             << ";frame=" << receipt.raw_frame_schema_version
             << ";developer=" << receipt.raw_developer_version
             << ";effective=" << raw_development_plan_identity(receipt.effective_plan);
    if (receipt.camera_profile_status != RawCameraProfileStatus::not_considered) {
        identity << ";camera-profile-status="
                 << camera_profile_status_name(receipt.camera_profile_status)
                 << ";camera-profile-catalog=" << receipt.camera_profile_catalog_identity
                 << ";camera-profile-developer="
                 << receipt.camera_profile_developer_version;
    }
    if (!receipt.camera_profile_identity.empty()) {
        identity << ";camera-profile=" << receipt.camera_profile_identity;
    }
    if (!receipt.fallback_reason.empty()) {
        identity << ";fallback=" << receipt.fallback_reason;
    }
    return identity.str();
}

DevelopedSourceReference develop_source_reference(
    const DecodeSession& session,
    const RawDevelopmentPlan& plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawPipelinePolicy& policy
) {
    return develop_source_reference(
        session,
        plan,
        preview_max_edge,
        policy,
        default_camera_profile_catalog()
    );
}

DevelopedSourceReference develop_source_reference(
    const DecodeSession& session,
    const RawDevelopmentPlan& plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawPipelinePolicy& policy,
    const CameraProfileCatalog& camera_profiles
) {
    if (
        policy.schema_version != raw_pipeline_policy_schema_version
        || plan.schema_version != raw_development_plan_schema_version
        || (preview_max_edge.has_value() && *preview_max_edge == 0U)
    ) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "source development received an invalid plan, policy, or preview edge"
        );
    }
    const bool is_raw = session.raw_development_capabilities().available;
    if (!is_raw) {
        if (policy.mode == RawPipelineMode::require_shadow_raw_frame) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "a decoded raster cannot enter the Bayer RawFrame developer"
            );
        }
        return provider_processed_source(
            session,
            plan,
            preview_max_edge,
            RawPipelinePath::decoded_raster,
            {}
        );
    }
    if (policy.mode == RawPipelineMode::require_provider_processed) {
        return provider_processed_source(
            session,
            plan,
            preview_max_edge,
            RawPipelinePath::provider_processed_compatibility,
            "provider processed path was explicitly selected"
        );
    }
    if (!session.capabilities().raw_frame) {
        if (policy.mode == RawPipelineMode::require_shadow_raw_frame) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "the selected RAW provider cannot expose an owned RawFrame"
            );
        }
        return provider_processed_source(
            session,
            plan,
            preview_max_edge,
            RawPipelinePath::provider_processed_compatibility,
            "provider does not expose an owned RawFrame"
        );
    }

    try {
        const auto negotiation = negotiate_shadow_raw_frame_plan(plan);
        if (!negotiation.accepted()) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "Shadow's RawFrame developer cannot honor the requested development plan"
            );
        }
        RawFrame frame = session.decode_raw_frame();
        const auto source_provider_id = frame.descriptor.provider_id;
        const auto source_provider_version = frame.descriptor.provider_version;
        const auto camera_profile = match_camera_profile(camera_profiles, session.metadata());
        std::optional<DcpColorTransform> dcp_transform;
        RawCameraProfileStatus camera_profile_status = RawCameraProfileStatus::no_match;
        std::string camera_profile_identity;
        std::string camera_profile_name;
        std::string camera_profile_diagnostic;
        if (camera_profile != nullptr) {
            camera_profile_identity = camera_profile->content_identity;
            camera_profile_name = camera_profile->profile.profile_name.empty()
                ? camera_profile->profile.unique_camera_model
                : camera_profile->profile.profile_name;
            try {
                dcp_transform = compile_dcp_color_transform(*camera_profile, frame.descriptor);
                camera_profile_status = RawCameraProfileStatus::applied;
            } catch (const DcpColorDevelopmentError& profile_error) {
                // Optional local profiles are an enhancement, not a prerequisite for decoding.
                // Reject the complete profile visibly and retain the generic provider matrix;
                // never apply only a matrix while silently dropping its creative tables.
                camera_profile_status = RawCameraProfileStatus::matched_not_applied;
                camera_profile_diagnostic = profile_error.what();
            }
        }
        DevelopedRawFrame developed = develop_raw_frame(
            std::move(frame),
            negotiation.effective,
            preview_max_edge,
            dcp_transform.has_value() ? &*dcp_transform : nullptr
        );
        PixelBuffer pixels = std::move(developed.pixels);
        pixels.raw_development_receipt.requested_plan = plan;
        pixels.raw_development_receipt.requested_plan_identity =
            raw_development_plan_identity(plan);
        pixels.raw_development_receipt.effective_plan = negotiation.effective;
        pixels.raw_development_receipt.effective_plan_identity =
            raw_development_plan_identity(negotiation.effective);
        pixels.raw_development_receipt.plan_negotiation_status = negotiation.status;
        RawPipelineReceipt pipeline;
        pipeline.path = RawPipelinePath::shadow_raw_frame;
        pipeline.pipeline_identity = std::string(raw_frame_pipeline_identity);
        pipeline.pipeline_identity += ";backend="
            + std::string(raw_development_backend_identity(developed.backend));
        pipeline.source_provider_id = source_provider_id;
        pipeline.source_provider_version = source_provider_version;
        pipeline.raw_frame_schema_version = raw_frame_schema_version;
        pipeline.raw_developer_version = shadow_raw_frame_developer_version;
        pipeline.requested_plan = plan;
        pipeline.effective_plan = negotiation.effective;
        pipeline.camera_profile_status = camera_profile_status;
        pipeline.camera_profile_catalog_identity = camera_profiles.identity;
        pipeline.camera_profile_identity = std::move(camera_profile_identity);
        pipeline.camera_profile_name = std::move(camera_profile_name);
        pipeline.camera_profile_diagnostic = std::move(camera_profile_diagnostic);
        pipeline.camera_profile_developer_version = dcp_color_developer_version;
        pipeline.pipeline_identity += ";camera-profile-status="
            + std::string(camera_profile_status_name(pipeline.camera_profile_status))
            + ";camera-profile-catalog=" + pipeline.camera_profile_catalog_identity;
        if (!pipeline.camera_profile_identity.empty()) {
            pipeline.pipeline_identity += ";camera-profile="
                + pipeline.camera_profile_identity;
        }
        if (!pipeline.valid()) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "Shadow RawFrame development produced an invalid pipeline receipt"
            );
        }
        return DevelopedSourceReference{
            .pixels = std::move(pixels),
            .pipeline_receipt = std::move(pipeline),
        };
    } catch (const DecodeError& error) {
        if (
            policy.mode == RawPipelineMode::require_shadow_raw_frame
            || !can_fallback_from(error)
        ) {
            throw;
        }
        return provider_processed_source(
            session,
            plan,
            preview_max_edge,
            RawPipelinePath::provider_processed_compatibility,
            error.what()
        );
    }
}

} // namespace shadow::image
