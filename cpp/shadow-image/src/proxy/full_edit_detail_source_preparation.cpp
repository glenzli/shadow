#include "full_edit_detail_source_preparation.hpp"

#include "developed_source_raster.hpp"
#include "proxy_render_request_validation.hpp"
#include "../raw/raw_foundation_source.hpp"

#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/source_rendering.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace shadow::image::proxy_detail {

bool full_detail_source_allows_metal_publication(
    const FullEditDetailSourceRequirements& requirements
) noexcept {
    return !requirements.requires_cpu_replay;
}

void validate_full_detail_source_preflight(
    const AssetMetadata& metadata,
    const FullDetailSourceStorage storage
) {
    const std::uint64_t pixels =
        std::max(metadata.raw_dimensions.pixel_count(), metadata.image_dimensions.pixel_count());
    const std::uint64_t retained_limit = storage == FullDetailSourceStorage::resident_raw_candidate
                                             ? maximum_full_edit_detail_retained_bytes
                                             : maximum_full_edit_scene_linear_retained_bytes;
    const std::uint64_t bytes_per_pixel = storage == FullDetailSourceStorage::resident_raw_candidate
                                              ? sizeof(std::uint16_t)
                                              : 3U * sizeof(float);
    if (pixels == 0U || pixels > retained_limit / bytes_per_pixel) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            storage == FullDetailSourceStorage::resident_raw_candidate
                ? "full edit detail metadata exceeds the 512 MiB resident CFA limit"
                : "full edit detail metadata exceeds the 1 GiB scene-linear RGB limit"
        );
    }
}

namespace {

[[nodiscard]] std::uint64_t checked_detail_retained_bytes(const PixelBuffer& source) {
    if (source.samples.capacity()
        > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail retained byte count overflows"
        );
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(source.samples.capacity()) * sizeof(std::uint16_t);
    if (bytes > maximum_full_edit_detail_retained_bytes) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail source exceeds the 512 MiB retained limit"
        );
    }
    return bytes;
}

[[nodiscard]] std::uint64_t checked_detail_retained_bytes(const SceneLinearRgbFrame& source) {
    validate_developed_source(source);
    const std::uint64_t bytes = static_cast<std::uint64_t>(source.samples.size()) * sizeof(float);
    if (bytes > maximum_full_edit_scene_linear_retained_bytes) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit scene-linear RAW source exceeds the 1 GiB retained-buffer limit"
        );
    }
    return bytes;
}

[[nodiscard]] std::uint64_t
checked_detail_retained_bytes(const raw_pipeline_detail::ResidentRawSource& source) {
    const std::uint64_t bytes = source.retained_bytes();
    if (bytes == 0U || bytes > maximum_full_edit_detail_retained_bytes) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit resident RAW source exceeds the 512 MiB retained limit"
        );
    }
    return bytes;
}

struct PreparedReferenceRgb final {
    DevelopedSourcePixels source;
    RawDevelopmentReceipt raw_development_receipt;
    RawPipelineReceipt raw_pipeline_receipt;
    OpticsProfileReceipt optics_receipt;
    SourceRenderingReceipt source_rendering;
};

[[nodiscard]] PreparedReferenceRgb finish_reference_rgb(
    DevelopedSourceReference developed,
    const DecodeSession& session,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    DevelopedSourcePixels source = std::move(developed.source);
    validate_developed_source(source);
    RawDevelopmentReceipt raw_development_receipt = std::move(developed.raw_development_receipt);
    const SourceRenderingReceipt source_rendering = std::visit(
        [&](const auto& value) {
            return resolve_source_rendering(value, session.metadata(), developed.pipeline_receipt);
        },
        source
    );
    OpticsProfileReceipt receipt;
    if (optics_provider == nullptr) {
        receipt.status = OpticsProfileStatus::disabled;
        receipt.provider_id = "none";
        receipt.provider_version = "none";
        return {
            .source = std::move(source),
            .raw_development_receipt = std::move(raw_development_receipt),
            .raw_pipeline_receipt = std::move(developed.pipeline_receipt),
            .optics_receipt = std::move(receipt),
            .source_rendering = source_rendering,
        };
    }
    if (std::holds_alternative<PixelBuffer>(source)) {
        auto corrected = optics_provider->correct_reference_rgb(
            std::get<PixelBuffer>(source),
            session.metadata(),
            optics_settings
        );
        receipt = std::move(corrected.receipt);
        if (corrected.corrected_reference_rgb.has_value()) {
            source = std::move(*corrected.corrected_reference_rgb);
        }
    } else {
        auto corrected = optics_provider->correct_scene_linear_reference(
            std::get<SceneLinearRgbFrame>(source),
            session.metadata(),
            optics_settings
        );
        receipt = std::move(corrected.receipt);
        if (corrected.corrected_scene_linear_rgb.has_value()) {
            source = std::move(*corrected.corrected_scene_linear_rgb);
        }
    }
    return {
        .source = std::move(source),
        .raw_development_receipt = std::move(raw_development_receipt),
        .raw_pipeline_receipt = std::move(developed.pipeline_receipt),
        .optics_receipt = std::move(receipt),
        .source_rendering = source_rendering,
    };
}

[[nodiscard]] PreparedReferenceRgb prepare_reference_rgb(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings,
    const RawPipelinePolicy& raw_policy,
    std::string raw_fallback_reason = {}
) {
    DevelopedSourceReference developed =
        develop_source_reference(session, raw_development_plan, std::nullopt, raw_policy);
    if (!raw_fallback_reason.empty()) {
        developed.pipeline_receipt.fallback_reason = std::move(raw_fallback_reason);
    }
    return finish_reference_rgb(std::move(developed), session, optics_provider, optics_settings);
}

[[nodiscard]] PreparedReferenceRgb prepare_reference_rgb(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings,
    const RawPipelinePolicy& raw_policy
) {
    return finish_reference_rgb(
        develop_source_reference(
            session,
            raw_development_plan,
            foundation,
            std::nullopt,
            raw_policy
        ),
        session,
        optics_provider,
        optics_settings
    );
}

[[nodiscard]] PreparedFullEditDetailSource
prepare_materialized_source(PreparedReferenceRgb reference) {
    const std::uint64_t retained_bytes = std::visit(
        [](const auto& value) { return checked_detail_retained_bytes(value); },
        reference.source
    );
    return PreparedFullEditDetailSource{
        .source = std::move(reference.source),
        .retained_bytes = retained_bytes,
        .raw_development_receipt = std::move(reference.raw_development_receipt),
        .raw_pipeline_receipt = std::move(reference.raw_pipeline_receipt),
        .optics_receipt = std::move(reference.optics_receipt),
        .source_rendering = std::move(reference.source_rendering),
    };
}

[[nodiscard]] PreparedFullEditDetailSource prepare_resident_source(
    raw_pipeline_detail::ResidentRawSource resident,
    const AssetMetadata& metadata
) {
    const std::uint64_t retained_bytes = checked_detail_retained_bytes(resident);
    const RawDevelopmentReceipt raw_receipt = resident.raw_development_receipt();
    const RawPipelineReceipt pipeline_receipt = resident.raw_pipeline_receipt();
    const OpticsProfileReceipt optics_receipt = resident.optics_receipt();
    const SourceRenderingReceipt source_rendering =
        resolve_source_rendering(metadata, pipeline_receipt);
    return PreparedFullEditDetailSource{
        .source = std::move(resident),
        .retained_bytes = retained_bytes,
        .raw_development_receipt = raw_receipt,
        .raw_pipeline_receipt = pipeline_receipt,
        .optics_receipt = optics_receipt,
        .source_rendering = source_rendering,
    };
}

[[nodiscard]] bool can_fallback_from_raw_frame(const DecodeError& error) noexcept {
    return error.code() == DecodeErrorCode::unsupported
           || error.code() == DecodeErrorCode::unsupported_layout;
}

[[nodiscard]] bool
raw_frame_route_candidate(const DecodeSession& session, const RawPipelinePolicy& policy) noexcept {
    return session.raw_development_capabilities().available && session.capabilities().raw_frame
           && policy.mode != RawPipelineMode::require_provider_processed;
}

} // namespace

PreparedFullEditDetailSource prepare_full_edit_detail_source(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
    const FullEditDetailSourceRequirements& requirements,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    if (raw_development_plan.intent != RawDevelopmentIntent::detail
        && raw_development_plan.intent != RawDevelopmentIntent::export_image) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "full-resolution edit source requires detail or export-image intent"
        );
    }
    validate_raw_development_plan_intent(
        raw_development_plan,
        raw_development_plan.intent,
        raw_development_plan.intent == RawDevelopmentIntent::detail ? "full edit detail"
                                                                    : "full image export"
    );
    const RawPipelinePolicy raw_policy = raw_pipeline_policy_from_environment();
    const bool raw_candidate = raw_frame_route_candidate(session, raw_policy);
    validate_full_detail_source_preflight(
        session.metadata(),
        raw_candidate ? FullDetailSourceStorage::resident_raw_candidate
                      : FullDetailSourceStorage::materialized_scene_linear
    );
    std::string raw_fallback_reason;
    if (raw_candidate) {
        std::optional<raw_pipeline_detail::PreparedRawFrameSource> prepared;
        try {
            prepared.emplace(
                raw_pipeline_detail::prepare_raw_frame_source(
                    session,
                    raw_development_plan,
                    std::nullopt,
                    default_camera_profile_catalog()
                )
            );
        } catch (const DecodeError& error) {
            if (raw_policy.mode == RawPipelineMode::require_shadow_raw_frame
                || !can_fallback_from_raw_frame(error)) {
                throw;
            }
            raw_fallback_reason = error.what();
        }
        if (prepared.has_value()) {
            detail::PreparedSceneLinearRegionOptics region_optics =
                prepared->prepare_region_optics(optics_provider, optics_settings);
            if (raw_pipeline_detail::cpu_resident_raw_source_supported(*prepared, region_optics)) {
                auto resident = raw_pipeline_detail::prepare_resident_raw_source(
                    std::move(*prepared),
                    std::move(region_optics)
                );
                return prepare_resident_source(std::move(resident), session.metadata());
            }
            if (prepared->development().requested_backend() != RawDevelopmentBackendMode::cpu
                && full_detail_source_allows_metal_publication(requirements)) {
                auto metal_resident = raw_pipeline_detail::try_prepare_metal_resident_raw_source(
                    std::move(*prepared),
                    std::move(region_optics)
                );
                if (metal_resident.published()) {
                    return prepare_resident_source(
                        std::move(*metal_resident.source),
                        session.metadata()
                    );
                }
                if (!metal_resident.fallback_source.has_value()) {
                    throw DecodeError(
                        DecodeErrorCode::internal,
                        0,
                        metal_resident.diagnostic.empty()
                            ? "Metal resident RAW preparation returned no source or fallback"
                            : std::move(metal_resident.diagnostic)
                    );
                }
                prepared.reset();
                prepared.emplace(std::move(*metal_resident.fallback_source));
            }
            validate_full_detail_source_preflight(
                session.metadata(),
                FullDetailSourceStorage::materialized_scene_linear
            );
            return prepare_materialized_source(finish_reference_rgb(
                raw_pipeline_detail::materialize_prepared_raw_frame_source(std::move(*prepared)),
                session,
                optics_provider,
                optics_settings
            ));
        }
    }
    RawPipelinePolicy fallback_policy = raw_policy;
    if (!raw_fallback_reason.empty()) {
        validate_full_detail_source_preflight(
            session.metadata(),
            FullDetailSourceStorage::materialized_scene_linear
        );
        fallback_policy.mode = RawPipelineMode::require_provider_processed;
    }
    return prepare_materialized_source(prepare_reference_rgb(
        session,
        raw_development_plan,
        optics_provider,
        optics_settings,
        fallback_policy,
        std::move(raw_fallback_reason)
    ));
}

PreparedFullEditDetailSource prepare_full_edit_detail_source(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    const FullEditDetailSourceRequirements& requirements,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    if (raw_development_plan.intent != RawDevelopmentIntent::detail
        && raw_development_plan.intent != RawDevelopmentIntent::export_image) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "AI RAW foundation source requires detail or export-image intent"
        );
    }
    validate_raw_development_plan_intent(
        raw_development_plan,
        raw_development_plan.intent,
        raw_development_plan.intent == RawDevelopmentIntent::detail
            ? "AI RAW foundation full edit detail"
            : "AI RAW foundation full image export"
    );
    static_cast<void>(requirements);
    validate_full_detail_source_preflight(
        session.metadata(),
        FullDetailSourceStorage::materialized_scene_linear
    );
    return prepare_materialized_source(prepare_reference_rgb(
        session,
        raw_development_plan,
        foundation,
        optics_provider,
        optics_settings,
        raw_pipeline_policy_from_environment()
    ));
}

PreparedFullEditDetailSource prepare_full_edit_detail_source(
    const DecodeSession& metadata_session,
    RawFrame staged_frame,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    const FullEditDetailSourceRequirements& requirements,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    if (raw_development_plan.intent != RawDevelopmentIntent::detail
        && raw_development_plan.intent != RawDevelopmentIntent::export_image) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "staged AI RAW foundation source requires detail or export-image intent"
        );
    }
    validate_raw_development_plan_intent(
        raw_development_plan,
        raw_development_plan.intent,
        raw_development_plan.intent == RawDevelopmentIntent::detail
            ? "staged AI RAW foundation full edit detail"
            : "staged AI RAW foundation full image export"
    );
    static_cast<void>(requirements);
    validate_full_detail_source_preflight(
        metadata_session.metadata(),
        FullDetailSourceStorage::materialized_scene_linear
    );
    const RawPipelinePolicy policy = raw_pipeline_policy_from_environment();
    if (policy.mode == RawPipelineMode::require_provider_processed) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "staged AI RAW foundation development is disabled by the RAW pipeline policy"
        );
    }
    RawDevelopmentPlan effective_plan = raw_development_plan;
    effective_plan.noise_reduction = RawNoiseReductionIntent::disabled;
    effective_plan.highlight_recovery = RawHighlightRecoveryIntent::disabled;
    auto prepared = raw_pipeline_detail::prepare_raw_frame_source(
        metadata_session,
        std::move(staged_frame),
        effective_plan,
        std::nullopt,
        default_camera_profile_catalog()
    );
    return prepare_materialized_source(finish_reference_rgb(
        raw_pipeline_detail::materialize_prepared_raw_foundation_source(
            std::move(prepared),
            foundation,
            raw_development_plan
        ),
        metadata_session,
        optics_provider,
        optics_settings
    ));
}

} // namespace shadow::image::proxy_detail
