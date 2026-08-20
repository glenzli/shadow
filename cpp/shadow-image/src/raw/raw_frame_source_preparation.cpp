#include "raw_frame_source_preparation.hpp"

#include "../optics/scene_linear_region_optics.hpp"
#include "raw_frame_region_development.hpp"
#include "raw_frame_source_development.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/decoder_error.hpp>

#include <optional>
#include <string>
#include <utility>

namespace shadow::image::raw_pipeline_detail {

namespace {

inline constexpr std::string_view raw_frame_pipeline_identity =
    "shadow-raw-frame-developer@20260821.1:cfa-wb-before-demosaic:decoder-matrix-dcp-neutral-only:"
    "bayer-area-preview+bayer-bilinear:raw-denoise-cfa-bilateral-v1:plan-camera-neutral:"
    "camera-matrix:scene-linear-f32";

[[nodiscard]] const char* camera_profile_status_name(const RawCameraProfileStatus status) noexcept {
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

} // namespace

PreparedRawFrameSource::PreparedRawFrameSource(
    RawFrame frame,
    PreparedRawFrameDevelopment development,
    RawPipelineReceipt pipeline,
    const RawDevelopmentPlanNegotiationStatus plan_negotiation_status,
    AssetMetadata metadata,
    std::optional<CameraProfileDefinition> camera_profile_definition
) :
    frame_(std::move(frame)), development_(std::move(development)), pipeline_(std::move(pipeline)),
    plan_negotiation_status_(plan_negotiation_status), metadata_(std::move(metadata)),
    camera_profile_definition_(std::move(camera_profile_definition)),
    region_optics_identity_(new detail::PreparedRegionOpticsSourceIdentity()) {}

const PreparedRawFrameDevelopment& PreparedRawFrameSource::development() const noexcept {
    return development_;
}

detail::PreparedSceneLinearRegionOptics PreparedRawFrameSource::prepare_region_optics(
    const OpticsProvider* provider,
    const OpticsSettings& settings
) const {
    const Dimensions output_dimensions = oriented_raw_dimensions(
        development_.reconstruction_dimensions(),
        development_.descriptor().orientation
    );
    auto optics = detail::prepare_scene_linear_region_optics(
        provider,
        output_dimensions,
        metadata_,
        settings
    );
    optics.source_identity_ = region_optics_identity_;
    return optics;
}

bool PreparedRawFrameSource::owns_region_optics(
    const detail::PreparedSceneLinearRegionOptics& optics
) const noexcept {
    return region_optics_identity_ != nullptr
           && optics.source_identity_.get() == region_optics_identity_.get();
}

PreparedRawFrameSource prepare_raw_frame_source(
    const DecodeSession& session,
    const RawDevelopmentPlan& requested_plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const CameraProfileCatalog& camera_profiles
) {
    return prepare_raw_frame_source(
        session,
        session.decode_raw_frame(),
        requested_plan,
        preview_max_edge,
        camera_profiles
    );
}

PreparedRawFrameSource prepare_raw_frame_source(
    const DecodeSession& session,
    RawFrame frame,
    const RawDevelopmentPlan& requested_plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const CameraProfileCatalog& camera_profiles
) {
    return prepare_raw_frame_source(
        session.metadata(),
        std::move(frame),
        requested_plan,
        preview_max_edge,
        camera_profiles
    );
}

PreparedRawFrameSource prepare_raw_frame_source(
    AssetMetadata metadata,
    RawFrame frame,
    const RawDevelopmentPlan& requested_plan,
    const std::optional<std::uint32_t> preview_max_edge,
    const CameraProfileCatalog& camera_profiles
) {
    const RawDevelopmentPlanNegotiation negotiation =
        negotiate_shadow_raw_frame_development_plan(requested_plan);
    if (!negotiation.accepted()) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Shadow's RawFrame developer cannot honor the requested development plan"
        );
    }
    RawPipelineReceipt pipeline;
    pipeline.path = RawPipelinePath::shadow_raw_frame;
    pipeline.source_provider_id = frame.descriptor.provider_id;
    pipeline.source_provider_version = frame.descriptor.provider_version;
    pipeline.raw_frame_schema_version = raw_frame_schema_version;
    pipeline.raw_developer_version = shadow_raw_frame_developer_version;
    pipeline.requested_plan = requested_plan;
    pipeline.effective_plan = negotiation.effective;
    pipeline.camera_profile_catalog_identity = camera_profiles.identity;
    pipeline.camera_profile_developer_version = dcp_color_developer_version;

    const CameraProfileDefinition* camera_profile =
        match_camera_profile(camera_profiles, metadata);
    std::optional<DcpColorTransform> dcp_transform;
    std::optional<CameraProfileDefinition> camera_profile_definition;
    pipeline.camera_profile_status = RawCameraProfileStatus::no_match;
    if (camera_profile != nullptr) {
        camera_profile_definition = *camera_profile;
        pipeline.camera_profile_identity = camera_profile->content_identity;
        pipeline.camera_profile_name = camera_profile->profile.profile_name.empty()
                                           ? camera_profile->profile.unique_camera_model
                                           : camera_profile->profile.profile_name;
        try {
            dcp_transform = compile_dcp_color_transform(
                *camera_profile,
                frame.descriptor,
                negotiation.effective.white_balance
            );
            pipeline.camera_profile_status = RawCameraProfileStatus::applied;
        } catch (const DcpColorDevelopmentError& profile_error) {
            // A matching optional profile remains auditable but cannot partially change the
            // generic provider matrix when any required stage cannot compile.
            pipeline.camera_profile_status = RawCameraProfileStatus::matched_not_applied;
            pipeline.camera_profile_diagnostic = profile_error.what();
        }
    }

    PreparedRawFrameDevelopment development = prepare_raw_frame_development(
        frame,
        negotiation.effective,
        preview_max_edge,
        std::move(dcp_transform),
        metadata.iso_speed
    );
    pipeline.source_scene_luminance_percentile = development.source_scene_luminance_percentile();
    return PreparedRawFrameSource(
        std::move(frame),
        std::move(development),
        std::move(pipeline),
        negotiation.status,
        std::move(metadata),
        std::move(camera_profile_definition)
    );
}

RawPipelineReceipt finalize_raw_frame_pipeline_receipt(
    RawPipelineReceipt prepared,
    const RawDevelopmentBackend backend,
    const RawHighlightRecoveryIntent highlight_recovery,
    const std::string_view raw_denoise_cache_identity
) {
    return finalize_raw_frame_pipeline_receipt(
        std::move(prepared),
        backend,
        highlight_recovery,
        raw_denoise_cache_identity,
        raw_frame_pipeline_identity
    );
}

RawPipelineReceipt finalize_raw_frame_pipeline_receipt(
    RawPipelineReceipt prepared,
    const RawDevelopmentBackend backend,
    const RawHighlightRecoveryIntent highlight_recovery,
    const std::string_view raw_denoise_cache_identity,
    const std::string_view source_stage_identity
) {
    if (source_stage_identity.empty()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Shadow RawFrame development requires a source-stage identity"
        );
    }
    prepared.pipeline_identity = std::string(source_stage_identity);
    prepared.pipeline_identity +=
        ";backend=" + std::string(raw_development_backend_identity(backend));
    prepared.pipeline_identity +=
        ";" + std::string(raw_highlight_treatment_identity(highlight_recovery));
    prepared.pipeline_identity += ";" + std::string(raw_denoise_cache_identity);
    prepared.pipeline_identity +=
        ";camera-profile-status="
        + std::string(camera_profile_status_name(prepared.camera_profile_status))
        + ";camera-profile-catalog=" + prepared.camera_profile_catalog_identity;
    if (!prepared.camera_profile_identity.empty()) {
        prepared.pipeline_identity += ";camera-profile=" + prepared.camera_profile_identity;
    }
    if (!prepared.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "Shadow RawFrame development produced an invalid pipeline receipt"
        );
    }
    return prepared;
}

DevelopedSourceReference materialize_prepared_raw_frame_source(PreparedRawFrameSource prepared) {
    const RawDevelopmentPlanNegotiationStatus plan_negotiation_status =
        prepared.plan_negotiation_status_;
    RawPipelineReceipt pipeline = std::move(prepared.pipeline_);
    DevelopedRawFrame developed = develop_raw_frame(prepared);
    RawDevelopmentReceipt raw_receipt = std::move(developed.raw_development_receipt);
    raw_receipt.requested_plan = pipeline.requested_plan;
    raw_receipt.requested_plan_identity = raw_development_plan_identity(pipeline.requested_plan);
    raw_receipt.effective_plan = pipeline.effective_plan;
    raw_receipt.effective_plan_identity = raw_development_plan_identity(pipeline.effective_plan);
    raw_receipt.plan_negotiation_status = plan_negotiation_status;
    pipeline = finalize_raw_frame_pipeline_receipt(
        std::move(pipeline),
        developed.backend,
        developed.highlight_recovery,
        developed.raw_denoise_cache_identity
    );
    return DevelopedSourceReference{
        .source = std::move(developed.source),
        .raw_development_receipt = std::move(raw_receipt),
        .pipeline_receipt = std::move(pipeline),
        .sensor_clipping_mask = std::move(developed.sensor_clipping_mask),
    };
}

} // namespace shadow::image::raw_pipeline_detail
