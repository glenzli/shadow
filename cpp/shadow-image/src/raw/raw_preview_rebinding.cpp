#include "raw_preview_rebinding.hpp"

#include "neural_raw_denoise/neural_raw_denoise.hpp"
#include "raw_denoise_plan.hpp"
#include "raw_foundation_source.hpp"
#include "raw_frame_development_plan.hpp"
#include "raw_frame_source_preparation.hpp"
#include "metal_raw_development.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace shadow::image::raw_pipeline_detail {

namespace {

struct OrdinaryRawPreviewBasis final {
    RawFrame denoised_frame;
    detail::NeuralRawDenoiseReceipt neural_denoise;
    RawBayerDenoiseReceipt conventional_denoise;
    std::string combined_denoise_identity;
};

struct FoundationRawPreviewBasis final {
    PreparedRawFoundationCameraRgb camera_rgb;
};

using RawPreviewBasis = std::variant<OrdinaryRawPreviewBasis, FoundationRawPreviewBasis>;

struct CompiledPreviewColorBinding final {
    RawFrameLinearTransform linear_transform;
    std::optional<DcpColorTransform> dcp;
    RawCameraProfileStatus camera_profile_status = RawCameraProfileStatus::no_match;
    std::string camera_profile_diagnostic;
};

[[nodiscard]] bool
same_plan_except_white_balance(RawDevelopmentPlan left, RawDevelopmentPlan right) noexcept {
    left.white_balance = {};
    right.white_balance = {};
    return left == right;
}

[[nodiscard]] CompiledPreviewColorBinding compile_color_binding(
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance,
    const std::optional<CameraProfileDefinition>& camera_profile_definition
) {
    CompiledPreviewColorBinding binding;
    if (camera_profile_definition.has_value()) {
        try {
            binding.dcp =
                compile_dcp_color_transform(*camera_profile_definition, descriptor, white_balance);
            binding.camera_profile_status = RawCameraProfileStatus::applied;
        } catch (const DcpColorDevelopmentError& error) {
            binding.camera_profile_status = RawCameraProfileStatus::matched_not_applied;
            binding.camera_profile_diagnostic = error.what();
        }
    }
    binding.linear_transform = prepare_raw_frame_linear_transform(
        descriptor,
        white_balance,
        binding.dcp.has_value() ? &*binding.dcp : nullptr
    );
    return binding;
}

[[nodiscard]] RawPipelineReceipt rebound_pipeline_template(
    const RawPipelineReceipt& original,
    const RawDevelopmentPlan& requested,
    const RawDevelopmentPlan& effective,
    const CompiledPreviewColorBinding& binding,
    const double source_scene_luminance_percentile
) {
    RawPipelineReceipt pipeline = original;
    pipeline.requested_plan = requested;
    pipeline.effective_plan = effective;
    pipeline.camera_profile_status = binding.camera_profile_status;
    pipeline.camera_profile_diagnostic = binding.camera_profile_diagnostic;
    pipeline.source_scene_luminance_percentile = source_scene_luminance_percentile;
    return pipeline;
}

} // namespace

struct RawPreviewRebindingSource::Impl final {
    PreparedRawFrameDevelopment development_template;
    RawPipelineReceipt pipeline_template;
    RawDevelopmentPlan requested_plan_template;
    RawDevelopmentPlanNegotiationStatus negotiation_status =
        RawDevelopmentPlanNegotiationStatus::rejected;
    AssetMetadata metadata;
    std::optional<CameraProfileDefinition> camera_profile_definition;
    SensorClippingMask sensor_clipping;
    RawPreviewBasis basis;
    std::atomic<std::uint64_t> bind_count{0U};
    std::atomic<std::uint64_t> ordinary_raw_bind_count{0U};
    std::atomic<std::uint64_t> ordinary_raw_metal_development_count{0U};
    std::atomic<std::uint64_t> ordinary_raw_cpu_development_count{0U};
    std::atomic<std::uint64_t> ordinary_raw_fused_dcp_bind_count{0U};
    std::atomic<std::uint64_t> foundation_camera_rgb_bind_count{0U};
    std::atomic<std::uint64_t> foundation_amount_bind_count{0U};
    std::atomic<std::uint64_t> dcp_metal_execution_count{0U};
    std::atomic<std::uint64_t> dcp_cpu_execution_count{0U};

    Impl(
        PreparedRawFrameDevelopment development,
        RawPipelineReceipt pipeline,
        const RawDevelopmentPlan requested_plan,
        const RawDevelopmentPlanNegotiationStatus negotiation,
        AssetMetadata source_metadata,
        std::optional<CameraProfileDefinition> profile,
        SensorClippingMask clipping,
        RawPreviewBasis preview_basis
    ) :
        development_template(std::move(development)), pipeline_template(std::move(pipeline)),
        requested_plan_template(requested_plan), negotiation_status(negotiation),
        metadata(std::move(source_metadata)), camera_profile_definition(std::move(profile)),
        sensor_clipping(std::move(clipping)), basis(std::move(preview_basis)) {}
};

RawPreviewRebindingSource::RawPreviewRebindingSource(std::unique_ptr<Impl> impl) :
    impl_(std::move(impl)) {}

RawPreviewRebindingSource::~RawPreviewRebindingSource() = default;

const AssetMetadata& RawPreviewRebindingSource::metadata() const noexcept {
    return impl_->metadata;
}

RawPreviewRebindingTelemetry RawPreviewRebindingSource::telemetry() const noexcept {
    return RawPreviewRebindingTelemetry{
        .bind_count = impl_->bind_count.load(std::memory_order_relaxed),
        .ordinary_raw_bind_count = impl_->ordinary_raw_bind_count.load(std::memory_order_relaxed),
        .ordinary_raw_metal_development_count =
            impl_->ordinary_raw_metal_development_count.load(std::memory_order_relaxed),
        .ordinary_raw_cpu_development_count =
            impl_->ordinary_raw_cpu_development_count.load(std::memory_order_relaxed),
        .ordinary_raw_fused_dcp_bind_count =
            impl_->ordinary_raw_fused_dcp_bind_count.load(std::memory_order_relaxed),
        .foundation_camera_rgb_bind_count =
            impl_->foundation_camera_rgb_bind_count.load(std::memory_order_relaxed),
        .foundation_amount_bind_count =
            impl_->foundation_amount_bind_count.load(std::memory_order_relaxed),
        .dcp_metal_execution_count =
            impl_->dcp_metal_execution_count.load(std::memory_order_relaxed),
        .dcp_cpu_execution_count =
            impl_->dcp_cpu_execution_count.load(std::memory_order_relaxed),
    };
}

DevelopedSourceReference
RawPreviewRebindingSource::bind(const RawDevelopmentPlan& requested_plan) const {
    return bind_impl(requested_plan, std::nullopt);
}

DevelopedSourceReference RawPreviewRebindingSource::bind_foundation_amount(
    const RawDevelopmentPlan& requested_plan,
    const std::uint8_t amount_percent
) const {
    return bind_impl(requested_plan, amount_percent);
}

bool RawPreviewRebindingSource::supports_foundation_amount_rebinding() const noexcept {
    const auto* foundation = std::get_if<FoundationRawPreviewBasis>(&impl_->basis);
    return foundation != nullptr && foundation->camera_rgb.supports_amount_rebinding();
}

DevelopedSourceReference RawPreviewRebindingSource::bind_impl(
    const RawDevelopmentPlan& requested_plan,
    const std::optional<std::uint8_t> foundation_amount_percent
) const {
    if (!same_plan_except_white_balance(requested_plan, impl_->requested_plan_template)
        || !valid_raw_white_balance(requested_plan.white_balance)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "RAW preview rebinding may change only the absolute white balance"
        );
    }

    RawDevelopmentPlan effective_plan = impl_->development_template.development_plan();
    effective_plan.white_balance = requested_plan.white_balance;
    CompiledPreviewColorBinding binding = compile_color_binding(
        impl_->development_template.descriptor(),
        effective_plan.white_balance,
        impl_->camera_profile_definition
    );
    PreparedRawFrameDevelopment rebound_development = impl_->development_template.rebind_color(
        effective_plan,
        binding.linear_transform,
        std::move(binding.dcp),
        impl_->development_template.source_scene_luminance_percentile()
    );
    impl_->bind_count.fetch_add(1U, std::memory_order_relaxed);

    if (const auto* ordinary = std::get_if<OrdinaryRawPreviewBasis>(&impl_->basis)) {
        if (foundation_amount_percent.has_value()) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "ordinary RAW preview cannot bind an AI foundation amount"
            );
        }
        const DcpColorTransform* dcp = rebound_development.camera_profile();
        const bool dcp_requested = dcp != nullptr && dcp->has_post_matrix_stages();
        // The initial RAW source development already folds DCP input rendering into its Metal
        // tile transaction. Do the same for a white-balance rebind: otherwise a bounded Metal
        // reconstruction is copied to the host, uploaded once more for DCP, then copied back
        // before the ordinary warm-preview upload. Declining this optional continuation keeps
        // the exact existing backend selector and staged DCP fallback intact.
        std::optional<FusedRawFrameDevelopment> developed;
        bool fused_dcp_applied = false;
        if (dcp_requested
            && rebound_development.requested_backend() != RawDevelopmentBackendMode::cpu) {
            auto fused_attempt = detail::try_develop_bayer_linear_srgb_f32_metal(
                ordinary->denoised_frame,
                rebound_development.linear_transform(),
                rebound_development.preview_max_edge(),
                effective_plan.highlight_recovery,
                effective_plan.quality,
                detail::MetalRawDevelopmentContinuations{
                    .dcp_color_transform = dcp,
                }
            );
            if (fused_attempt.development.has_value() && fused_attempt.dcp_applied) {
                developed = std::move(fused_attempt.development);
                fused_dcp_applied = true;
            }
        }
        if (!developed.has_value()) {
            developed = develop_bayer_linear_srgb_f32_fused_with_backend(
                ordinary->denoised_frame,
                rebound_development.linear_transform(),
                rebound_development.preview_max_edge(),
                rebound_development.requested_backend(),
                effective_plan.highlight_recovery,
                effective_plan.quality
            );
        }
        impl_->ordinary_raw_bind_count.fetch_add(1U, std::memory_order_relaxed);
        if (developed->backend == RawDevelopmentBackend::metal) {
            impl_->ordinary_raw_metal_development_count.fetch_add(1U, std::memory_order_relaxed);
        } else {
            impl_->ordinary_raw_cpu_development_count.fetch_add(1U, std::memory_order_relaxed);
        }
        DcpColorExecutionBackend dcp_backend = fused_dcp_applied
            ? DcpColorExecutionBackend::metal
            : DcpColorExecutionBackend::cpu;
        if (fused_dcp_applied) {
            impl_->ordinary_raw_fused_dcp_bind_count.fetch_add(1U, std::memory_order_relaxed);
            impl_->dcp_metal_execution_count.fetch_add(1U, std::memory_order_relaxed);
        } else if (dcp_requested) {
            dcp_backend = apply_dcp_color_rendering_stages(developed->scene_linear, *dcp);
            if (dcp_backend == DcpColorExecutionBackend::metal) {
                impl_->dcp_metal_execution_count.fetch_add(1U, std::memory_order_relaxed);
            } else {
                impl_->dcp_cpu_execution_count.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        RawDevelopmentReceipt receipt = finalize_raw_frame_development_receipt(
            rebound_development,
            developed->scene_linear.dimensions,
            developed->demosaic_receipt,
            developed->backend,
            ordinary->neural_denoise,
            ordinary->conventional_denoise,
            dcp_backend
        );
        receipt.requested_plan = requested_plan;
        receipt.requested_plan_identity = raw_development_plan_identity(requested_plan);
        receipt.effective_plan = effective_plan;
        receipt.effective_plan_identity = raw_development_plan_identity(effective_plan);
        receipt.plan_negotiation_status = impl_->negotiation_status;

        RawPipelineReceipt pipeline = rebound_pipeline_template(
            impl_->pipeline_template,
            requested_plan,
            effective_plan,
            binding,
            rebound_development.source_scene_luminance_percentile()
        );
        pipeline = finalize_raw_frame_pipeline_receipt(
            std::move(pipeline),
            developed->backend,
            effective_plan.highlight_recovery,
            ordinary->combined_denoise_identity
        );
        return DevelopedSourceReference{
            .source = std::move(developed->scene_linear),
            .raw_development_receipt = std::move(receipt),
            .pipeline_receipt = std::move(pipeline),
            .sensor_clipping_mask = impl_->sensor_clipping,
        };
    }

    const auto& foundation = std::get<FoundationRawPreviewBasis>(impl_->basis);
    impl_->foundation_camera_rgb_bind_count.fetch_add(1U, std::memory_order_relaxed);
    if (foundation_amount_percent.has_value()) {
        impl_->foundation_amount_bind_count.fetch_add(1U, std::memory_order_relaxed);
    }
    DevelopedRawFoundation developed = foundation_amount_percent.has_value()
                                           ? develop_prepared_raw_foundation(
                                                 foundation.camera_rgb,
                                                 rebound_development.linear_transform(),
                                                 *foundation_amount_percent
                                             )
                                           : develop_prepared_raw_foundation(
                                                 foundation.camera_rgb,
                                                 rebound_development.linear_transform()
                                             );
    DcpColorExecutionBackend dcp_backend = DcpColorExecutionBackend::cpu;
    const DcpColorTransform* dcp = rebound_development.camera_profile();
    if (dcp != nullptr && dcp->has_post_matrix_stages()) {
        dcp_backend = apply_dcp_color_rendering_stages(developed.scene_linear, *dcp);
        if (dcp_backend == DcpColorExecutionBackend::metal) {
            impl_->dcp_metal_execution_count.fetch_add(1U, std::memory_order_relaxed);
        } else {
            impl_->dcp_cpu_execution_count.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    const auto foundation_negotiation = requested_plan == effective_plan
                                            ? RawDevelopmentPlanNegotiationStatus::accepted
                                            : RawDevelopmentPlanNegotiationStatus::adjusted;
    RawDevelopmentReceipt receipt = finalize_raw_foundation_receipt(
        rebound_development,
        requested_plan,
        foundation_negotiation,
        developed.scene_linear.dimensions,
        developed.bounded_preview,
        developed.cache_identity,
        dcp_backend
    );
    RawPipelineReceipt pipeline = rebound_pipeline_template(
        impl_->pipeline_template,
        requested_plan,
        effective_plan,
        binding,
        rebound_development.source_scene_luminance_percentile()
    );
    pipeline = finalize_raw_foundation_pipeline_receipt(
        std::move(pipeline),
        effective_plan.highlight_recovery,
        developed.cache_identity
    );
    return DevelopedSourceReference{
        .source = std::move(developed.scene_linear),
        .raw_development_receipt = std::move(receipt),
        .pipeline_receipt = std::move(pipeline),
        .sensor_clipping_mask = impl_->sensor_clipping,
    };
}

PreparedRawPreviewRebinding prepare_raw_preview_rebinding(PreparedRawFrameSource prepared) {
    const RawDevelopmentPlan requested_plan = prepared.pipeline_.requested_plan;
    SensorClippingMask sensor_clipping = project_sensor_clipping_mask(
        prepared.frame_,
        prepared.development_.diagnostic_dimensions()
    );
    detail::NeuralRawDenoiseResult neural = detail::execute_prepared_neural_raw_denoise(
        std::move(prepared.frame_),
        prepared.development_.neural_raw_denoise()
    );
    RawBayerDenoiseResult conventional = detail::execute_prepared_raw_bayer_denoise(
        std::move(neural.frame),
        prepared.development_.raw_denoise()
    );
    OrdinaryRawPreviewBasis basis{
        .denoised_frame = std::move(conventional.frame),
        .neural_denoise = std::move(neural.receipt),
        .conventional_denoise = std::move(conventional.receipt),
    };
    basis.combined_denoise_identity = detail::combined_raw_denoise_cache_identity(
        basis.neural_denoise,
        basis.conventional_denoise
    );
    auto impl = std::make_unique<RawPreviewRebindingSource::Impl>(
        std::move(prepared.development_),
        std::move(prepared.pipeline_),
        requested_plan,
        prepared.plan_negotiation_status_,
        std::move(prepared.metadata_),
        std::move(prepared.camera_profile_definition_),
        std::move(sensor_clipping),
        RawPreviewBasis{std::move(basis)}
    );
    auto source = std::shared_ptr<const RawPreviewRebindingSource>(
        new RawPreviewRebindingSource(std::move(impl))
    );
    return PreparedRawPreviewRebinding{
        .source = source,
        .developed = source->bind(requested_plan),
    };
}

PreparedRawPreviewRebinding prepare_raw_foundation_preview_rebinding(
    PreparedRawFrameSource prepared,
    const RawFoundationCameraRgbView& foundation,
    const RawDevelopmentPlan& requested_plan
) {
    if (!foundation.matches_source(prepared.development_.descriptor())) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "verified AI RAW foundation does not match the decoded source geometry"
        );
    }
    PreparedRawFoundationCameraRgb camera_rgb = prepare_raw_foundation_camera_rgb(
        foundation,
        prepared.frame_,
        prepared.development_.preview_max_edge()
    );
    SensorClippingMask sensor_clipping =
        project_sensor_clipping_mask(prepared.frame_, camera_rgb.dimensions);
    prepared.pipeline_.requested_plan = requested_plan;
    prepared.pipeline_.effective_plan = prepared.development_.development_plan();
    const RawDevelopmentPlanNegotiationStatus negotiation_status =
        requested_plan == prepared.pipeline_.effective_plan
            ? RawDevelopmentPlanNegotiationStatus::accepted
            : RawDevelopmentPlanNegotiationStatus::adjusted;
    auto impl = std::make_unique<RawPreviewRebindingSource::Impl>(
        std::move(prepared.development_),
        std::move(prepared.pipeline_),
        requested_plan,
        negotiation_status,
        std::move(prepared.metadata_),
        std::move(prepared.camera_profile_definition_),
        std::move(sensor_clipping),
        RawPreviewBasis{FoundationRawPreviewBasis{.camera_rgb = std::move(camera_rgb)}}
    );
    auto source = std::shared_ptr<const RawPreviewRebindingSource>(
        new RawPreviewRebindingSource(std::move(impl))
    );
    return PreparedRawPreviewRebinding{
        .source = source,
        .developed = source->bind(requested_plan),
    };
}

std::optional<PreparedRawPreviewRebinding> try_prepare_raw_preview_rebinding(
    const DecodeSession& session,
    const RawDevelopmentPlan& requested_plan,
    const std::uint32_t max_edge,
    const RawPipelinePolicy& policy,
    const CameraProfileCatalog& camera_profiles
) {
    if (max_edge == 0U || policy.schema_version != raw_pipeline_policy_schema_version
        || requested_plan.schema_version != raw_development_plan_schema_version) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "RAW preview rebinding received an invalid plan, policy, or preview edge"
        );
    }
    if (!session.raw_development_capabilities().available
        || policy.mode == RawPipelineMode::require_provider_processed
        || !session.capabilities().raw_frame) {
        return std::nullopt;
    }
    try {
        return prepare_raw_preview_rebinding(
            prepare_raw_frame_source(session, requested_plan, max_edge, camera_profiles)
        );
    } catch (const DecodeError&) {
        if (policy.mode == RawPipelineMode::require_shadow_raw_frame) {
            throw;
        }
        return std::nullopt;
    }
}

PreparedRawPreviewRebinding prepare_raw_foundation_preview_rebinding(
    const DecodeSession& session,
    const RawDevelopmentPlan& requested_plan,
    const RawFoundationCameraRgbView& foundation,
    const std::uint32_t max_edge,
    const RawPipelinePolicy& policy,
    const CameraProfileCatalog& camera_profiles
) {
    if (max_edge == 0U || policy.schema_version != raw_pipeline_policy_schema_version
        || requested_plan.schema_version != raw_development_plan_schema_version
        || !foundation.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "AI RAW preview rebinding received an invalid plan, policy, edge, or foundation"
        );
    }
    if (policy.mode == RawPipelineMode::require_provider_processed
        || !session.raw_development_capabilities().available || !session.capabilities().raw_frame) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "AI RAW preview rebinding requires an owned RawFrame source"
        );
    }
    RawDevelopmentPlan effective_plan = requested_plan;
    effective_plan.noise_reduction = RawNoiseReductionIntent::disabled;
    effective_plan.highlight_recovery = RawHighlightRecoveryIntent::disabled;
    return prepare_raw_foundation_preview_rebinding(
        prepare_raw_frame_source(session, effective_plan, max_edge, camera_profiles),
        foundation,
        requested_plan
    );
}

PreparedRawPreviewRebinding prepare_raw_foundation_preview_rebinding(
    const DecodeSession& metadata_session,
    RawFrame staged_frame,
    const RawDevelopmentPlan& requested_plan,
    const RawFoundationCameraRgbView& foundation,
    const std::uint32_t max_edge,
    const RawPipelinePolicy& policy,
    const CameraProfileCatalog& camera_profiles
) {
    if (max_edge == 0U || policy.schema_version != raw_pipeline_policy_schema_version
        || requested_plan.schema_version != raw_development_plan_schema_version
        || !foundation.valid() || !staged_frame.is_bayer_2x2()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "staged AI RAW preview rebinding received an invalid frame, plan, policy, edge, or "
            "foundation"
        );
    }
    if (policy.mode == RawPipelineMode::require_provider_processed) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "staged AI RAW preview rebinding is disabled by the RAW pipeline policy"
        );
    }
    RawDevelopmentPlan effective_plan = requested_plan;
    effective_plan.noise_reduction = RawNoiseReductionIntent::disabled;
    effective_plan.highlight_recovery = RawHighlightRecoveryIntent::disabled;
    return prepare_raw_foundation_preview_rebinding(
        prepare_raw_frame_source(
            metadata_session,
            std::move(staged_frame),
            effective_plan,
            max_edge,
            camera_profiles
        ),
        foundation,
        requested_plan
    );
}

} // namespace shadow::image::raw_pipeline_detail
