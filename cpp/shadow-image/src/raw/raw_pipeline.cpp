#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/dcp_color_development.hpp>

#include "raw_frame_source_development.hpp"

#include <cmath>
#include <cstdlib>
#include <iomanip>
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
    "raw-denoise-cfa-bilateral-v1:as-shot-neutral:camera-matrix:scene-linear-f32";

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
    RawDevelopmentReceipt raw_development_receipt = pixels.raw_development_receipt;
    return DevelopedSourceReference{
        .source = std::move(pixels),
        .raw_development_receipt = std::move(raw_development_receipt),
        .pipeline_receipt = std::move(pipeline),
    };
}

[[nodiscard]] bool can_fallback_from(const DecodeError& error) noexcept {
    return error.code() == DecodeErrorCode::unsupported
        || error.code() == DecodeErrorCode::unsupported_layout;
}

} // namespace

RawDevelopmentCapabilities shadow_raw_frame_development_capabilities() noexcept {
    RawDevelopmentCapabilities capabilities;
    capabilities.available = true;
    capabilities.raw_frame = true;
    capabilities.supported_intents =
        raw_development_intent_mask(RawDevelopmentIntent::preview)
        | raw_development_intent_mask(RawDevelopmentIntent::detail)
        | raw_development_intent_mask(RawDevelopmentIntent::export_image);
    capabilities.supported_qualities =
        raw_development_quality_mask(RawDevelopmentQuality::balanced)
        | raw_development_quality_mask(RawDevelopmentQuality::high);
    capabilities.supported_dng_opcode_policies =
        dng_opcode_policy_mask(DngOpcodePolicy::provider_default);
    capabilities.supported_noise_reduction_intents =
        raw_noise_reduction_intent_mask(RawNoiseReductionIntent::provider_default)
        | raw_noise_reduction_intent_mask(RawNoiseReductionIntent::disabled)
        | raw_noise_reduction_intent_mask(RawNoiseReductionIntent::conservative)
        | raw_noise_reduction_intent_mask(RawNoiseReductionIntent::noise_robust);
    capabilities.supported_highlight_recovery_intents =
        raw_highlight_recovery_intent_mask(RawHighlightRecoveryIntent::provider_default)
        | raw_highlight_recovery_intent_mask(RawHighlightRecoveryIntent::disabled);
    return capabilities;
}

RawDevelopmentPlanNegotiation negotiate_shadow_raw_frame_development_plan(
    const RawDevelopmentPlan& requested
) noexcept {
    return shadow::image::negotiate_raw_development_plan(
        requested,
        shadow_raw_frame_development_capabilities()
    );
}

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
            && fallback_reason.empty() && source_scene_luminance_percentile.has_value()
            && std::isfinite(*source_scene_luminance_percentile)
            && *source_scene_luminance_percentile >= 0.0;
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
        && !source_scene_luminance_percentile.has_value()
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
    if (receipt.source_scene_luminance_percentile.has_value()) {
        identity << ";source-luminance-p99=" << std::fixed << std::setprecision(8)
                 << *receipt.source_scene_luminance_percentile;
    }
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
        const auto negotiation = negotiate_shadow_raw_frame_development_plan(plan);
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
                // Keep the generic provider matrix and record the full diagnostic rather than
                // silently claiming camera rendering that the local DCP could not compile.
                camera_profile_status = RawCameraProfileStatus::matched_not_applied;
                camera_profile_diagnostic = profile_error.what();
            }
        }
        raw_pipeline_detail::DevelopedRawFrame developed =
            raw_pipeline_detail::develop_raw_frame(
                std::move(frame),
                negotiation.effective,
                preview_max_edge,
                dcp_transform.has_value() ? &*dcp_transform : nullptr,
                session.metadata().iso_speed
            );
        RawDevelopmentReceipt raw_development_receipt = std::move(
            developed.raw_development_receipt
        );
        raw_development_receipt.requested_plan = plan;
        raw_development_receipt.requested_plan_identity =
            raw_development_plan_identity(plan);
        raw_development_receipt.effective_plan = negotiation.effective;
        raw_development_receipt.effective_plan_identity =
            raw_development_plan_identity(negotiation.effective);
        raw_development_receipt.plan_negotiation_status = negotiation.status;
        RawPipelineReceipt pipeline;
        pipeline.path = RawPipelinePath::shadow_raw_frame;
        pipeline.pipeline_identity = std::string(raw_frame_pipeline_identity);
        pipeline.pipeline_identity += ";backend="
            + std::string(raw_development_backend_identity(developed.backend));
        pipeline.pipeline_identity += ";"
            + std::string(raw_highlight_treatment_identity(developed.highlight_recovery));
        pipeline.pipeline_identity += ";" + developed.raw_denoise_cache_identity;
        pipeline.source_provider_id = source_provider_id;
        pipeline.source_provider_version = source_provider_version;
        pipeline.raw_frame_schema_version = raw_frame_schema_version;
        pipeline.raw_developer_version = shadow_raw_frame_developer_version;
        pipeline.source_scene_luminance_percentile =
            developed.source_scene_luminance_percentile;
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
            .source = std::move(developed.source),
            .raw_development_receipt = std::move(raw_development_receipt),
            .pipeline_receipt = std::move(pipeline),
            .sensor_clipping_mask = std::move(developed.sensor_clipping_mask),
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
