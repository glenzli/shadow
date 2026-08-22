#include "raw_preview_rebinding.hpp"

#include "bayer_sampling.hpp"
#include "metal_raw_development.hpp"
#include "raw_denoise_plan.hpp"
#include "raw_foundation_source.hpp"
#include "raw_frame_development_plan.hpp"
#include "raw_frame_source_preparation.hpp"
#include "raw_source_reconstruction.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_white_balance.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image::raw_pipeline_detail {

namespace {

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
            // Keep the rebind path identical to initial RawFrame preparation. A decoder-provided
            // primary matrix may use a different output basis from the DCP, so only borrow the
            // DCP's native camera neutral for manual WB and do not reactivate its post-matrix
            // HueSat/Look/Tone stages on every slider update.
            if (descriptor.has_camera_to_linear_srgb_d65) {
                binding.dcp->clear_post_matrix_stages();
            }
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

[[nodiscard]] bool interactive_timing_enabled() noexcept {
    const char* value = std::getenv("SHADOW_INTERACTIVE_TIMING");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

void log_interactive_rebind_timing(
    const bool enabled,
    const std::uint64_t sequence,
    const char* const stage,
    const std::chrono::steady_clock::time_point started
) noexcept {
    if (!enabled) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    );
    std::fprintf(
        stderr,
        "shadow.interactive-timing component=native-raw-rebind sequence=%llu stage=%s "
        "elapsed_ms=%lld\n",
        static_cast<unsigned long long>(sequence),
        stage,
        static_cast<long long>(elapsed.count())
    );
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
    SourceReconstructionBasis basis;
#if SHADOW_IMAGE_HAS_METAL
    // This buffer is an acceleration cache only.  It retains the already-denoised CFA plane and
    // runs the same preview reconstruction kernel; the CPU frame remains the exact fallback.
    std::optional<detail::MetalRawPreviewRebindingSource> ordinary_raw_metal_preview;
#endif
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
        SourceReconstructionBasis preview_basis
#if SHADOW_IMAGE_HAS_METAL
        ,
        std::optional<detail::MetalRawPreviewRebindingSource> metal_preview
#endif
    ) :
        development_template(std::move(development)), pipeline_template(std::move(pipeline)),
        requested_plan_template(requested_plan), negotiation_status(negotiation),
        metadata(std::move(source_metadata)), camera_profile_definition(std::move(profile)),
        basis(std::move(preview_basis))
#if SHADOW_IMAGE_HAS_METAL
        ,
        ordinary_raw_metal_preview(std::move(metal_preview)) {
    }
#else
    {
    }
#endif
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
        .dcp_cpu_execution_count = impl_->dcp_cpu_execution_count.load(std::memory_order_relaxed),
    };
}

DevelopedSourceReference
RawPreviewRebindingSource::bind(const RawDevelopmentPlan& requested_plan) const {
    return bind_impl(requested_plan, std::nullopt);
}

std::optional<ResidentRawPreviewRebinding>
RawPreviewRebindingSource::try_bind_metal_resident(const RawDevelopmentPlan& requested_plan) const {
#if !SHADOW_IMAGE_HAS_METAL
    static_cast<void>(requested_plan);
    return std::nullopt;
#else
    if (!same_plan_except_white_balance(requested_plan, impl_->requested_plan_template)
        || !valid_raw_white_balance(requested_plan.white_balance)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "RAW preview rebinding may change only the absolute white balance"
        );
    }
    const auto* ordinary = std::get_if<SensorCfaSourceReconstructionBasis>(&impl_->basis);
    if (ordinary == nullptr || !impl_->ordinary_raw_metal_preview.has_value()) {
        return std::nullopt;
    }
    RawDevelopmentPlan effective_plan = impl_->development_template.development_plan();
    effective_plan.white_balance = requested_plan.white_balance;
    if (impl_->development_template.requested_backend() == RawDevelopmentBackendMode::cpu) {
        return std::nullopt;
    }
    const bool timing_enabled = interactive_timing_enabled();
    const auto timing_started = std::chrono::steady_clock::now();
    const auto timing_sequence = impl_->bind_count.load(std::memory_order_relaxed) + 1U;
    CompiledPreviewColorBinding binding = compile_color_binding(
        impl_->development_template.descriptor(),
        effective_plan.white_balance,
        impl_->camera_profile_definition
    );
    log_interactive_rebind_timing(
        timing_enabled,
        timing_sequence,
        "color-binding-ready",
        timing_started
    );
    PreparedRawFrameDevelopment rebound_development = impl_->development_template.rebind_color(
        effective_plan,
        binding.linear_transform,
        std::move(binding.dcp),
        impl_->development_template.source_scene_luminance_percentile()
    );
    log_interactive_rebind_timing(
        timing_enabled,
        timing_sequence,
        "development-plan-ready",
        timing_started
    );
    const DcpColorTransform* dcp = rebound_development.camera_profile();
    const bool dcp_requested = dcp != nullptr && dcp->has_post_matrix_stages();
    auto development = impl_->ordinary_raw_metal_preview->develop_resident(
        ordinary->denoised_frame,
        rebound_development.linear_transform(),
        rebound_development.preview_max_edge(),
        effective_plan.highlight_recovery,
        effective_plan.quality,
        detail::MetalRawDevelopmentContinuations{
            .dcp_color_transform = dcp,
        }
    );
    log_interactive_rebind_timing(
        timing_enabled,
        timing_sequence,
        "metal-development-ready",
        timing_started
    );
    if (!development.output.has_value() || (dcp_requested && !development.dcp_applied)) {
        log_interactive_rebind_timing(
            timing_enabled,
            timing_sequence,
            "metal-development-unavailable",
            timing_started
        );
        return std::nullopt;
    }
    const DcpColorExecutionBackend dcp_backend =
        development.dcp_applied ? DcpColorExecutionBackend::metal : DcpColorExecutionBackend::cpu;
    RawDevelopmentReceipt receipt = finalize_raw_frame_development_receipt(
        rebound_development,
        development.output->dimensions(),
        development.demosaic_receipt,
        RawDevelopmentBackend::metal,
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
        RawDevelopmentBackend::metal,
        effective_plan.highlight_recovery,
        ordinary->combined_denoise_identity
    );
    impl_->bind_count.fetch_add(1U, std::memory_order_relaxed);
    impl_->ordinary_raw_bind_count.fetch_add(1U, std::memory_order_relaxed);
    impl_->ordinary_raw_metal_development_count.fetch_add(1U, std::memory_order_relaxed);
    if (development.dcp_applied) {
        impl_->ordinary_raw_fused_dcp_bind_count.fetch_add(1U, std::memory_order_relaxed);
        impl_->dcp_metal_execution_count.fetch_add(1U, std::memory_order_relaxed);
    }
    log_interactive_rebind_timing(timing_enabled, timing_sequence, "receipt-ready", timing_started);
    return ResidentRawPreviewRebinding{
        .output = std::move(*development.output),
        .raw_development_receipt = std::move(receipt),
        .pipeline_receipt = std::move(pipeline),
        .sensor_clipping_mask = source_reconstruction_sensor_clipping(impl_->basis),
    };
#endif
}

DevelopedSourceReference RawPreviewRebindingSource::bind_foundation_amount(
    const RawDevelopmentPlan& requested_plan,
    const std::uint8_t amount_percent
) const {
    return bind_impl(requested_plan, amount_percent);
}

bool RawPreviewRebindingSource::supports_foundation_amount_rebinding() const noexcept {
    const auto* foundation = std::get_if<AiCameraRgbSourceReconstructionBasis>(&impl_->basis);
    return foundation != nullptr && foundation->camera_rgb.supports_amount_rebinding();
}

bool RawPreviewRebindingSource::supports_raw_white_balance_picker() const noexcept {
    return source_reconstruction_retains_sensor_cfa(impl_->basis);
}

std::optional<RawWhiteBalancePresentation> RawPreviewRebindingSource::pick_raw_white_balance(
    const double normalized_x,
    const double normalized_y
) const noexcept {
    if (!std::isfinite(normalized_x) || !std::isfinite(normalized_y) || normalized_x < 0.0
        || normalized_x > 1.0 || normalized_y < 0.0 || normalized_y > 1.0) {
        return std::nullopt;
    }
    const auto* ordinary = std::get_if<SensorCfaSourceReconstructionBasis>(&impl_->basis);
    if (ordinary == nullptr) {
        return std::nullopt;
    }
    try {
        const RawFrame& frame = ordinary->denoised_frame;
        const auto& descriptor = frame.descriptor;
        if (!frame.valid() || descriptor.active_dimensions.width < 3U
            || descriptor.active_dimensions.height < 3U) {
            return std::nullopt;
        }

        const bool rotated = descriptor.orientation == 5 || descriptor.orientation == 6;
        const auto display_width =
            rotated ? descriptor.active_dimensions.height : descriptor.active_dimensions.width;
        const auto display_height =
            rotated ? descriptor.active_dimensions.width : descriptor.active_dimensions.height;
        const auto active_x = static_cast<std::uint32_t>(
            std::llround(normalized_x * static_cast<double>(display_width - 1U))
        );
        const auto active_y = static_cast<std::uint32_t>(
            std::llround(normalized_y * static_cast<double>(display_height - 1U))
        );
        std::uint32_t source_x = active_x;
        std::uint32_t source_y = active_y;
        switch (descriptor.orientation) {
        case 0:
            break;
        case 3:
            source_x = descriptor.active_dimensions.width - 1U - active_x;
            source_y = descriptor.active_dimensions.height - 1U - active_y;
            break;
        case 5:
            source_x = descriptor.active_dimensions.width - 1U - active_y;
            source_y = active_x;
            break;
        case 6:
            source_x = active_y;
            source_y = descriptor.active_dimensions.height - 1U - active_x;
            break;
        default:
            return std::nullopt;
        }
        if (source_x >= descriptor.active_dimensions.width
            || source_y >= descriptor.active_dimensions.height) {
            return std::nullopt;
        }
        const auto raw_x = descriptor.active_margins.left + source_x;
        const auto raw_y = descriptor.active_margins.top + source_y;
        std::array<std::vector<float>, 3U> channel_values;
        for (std::int32_t delta_y = -2; delta_y <= 2; ++delta_y) {
            for (std::int32_t delta_x = -2; delta_x <= 2; ++delta_x) {
                const auto candidate_x = static_cast<std::int64_t>(raw_x) + delta_x;
                const auto candidate_y = static_cast<std::int64_t>(raw_y) + delta_y;
                if (candidate_x < 0 || candidate_y < 0
                    || candidate_x >= static_cast<std::int64_t>(descriptor.storage_dimensions.width)
                    || candidate_y
                           >= static_cast<std::int64_t>(descriptor.storage_dimensions.height)) {
                    continue;
                }
                const auto sample = detail::bilinear_camera_rgb_sample_at(
                    frame,
                    static_cast<std::uint32_t>(candidate_x),
                    static_cast<std::uint32_t>(candidate_y),
                    nullptr
                );
                if (!std::isfinite(sample.values[0]) || !std::isfinite(sample.values[1])
                    || !std::isfinite(sample.values[2]) || sample.values[0] <= 1.0e-6F
                    || sample.values[1] <= 1.0e-6F || sample.values[2] <= 1.0e-6F) {
                    continue;
                }
                for (std::size_t channel = 0U; channel < channel_values.size(); ++channel) {
                    channel_values[channel].push_back(sample.values[channel]);
                }
            }
        }
        std::array<double, 3U> neutral{};
        for (std::size_t channel = 0U; channel < channel_values.size(); ++channel) {
            auto& values = channel_values[channel];
            if (values.empty()) {
                return std::nullopt;
            }
            std::sort(values.begin(), values.end());
            neutral[channel] = values[values.size() / 2U];
        }
        if (impl_->camera_profile_definition.has_value()) {
            return raw_dcp_white_balance_presentation(
                impl_->camera_profile_definition->profile,
                neutral
            );
        }
        return raw_frame_white_balance_presentation(descriptor, neutral);
    } catch (...) {
        return std::nullopt;
    }
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
    const bool timing_enabled = interactive_timing_enabled();
    const auto timing_started = std::chrono::steady_clock::now();
    const auto timing_sequence = impl_->bind_count.load(std::memory_order_relaxed) + 1U;
    CompiledPreviewColorBinding binding = compile_color_binding(
        impl_->development_template.descriptor(),
        effective_plan.white_balance,
        impl_->camera_profile_definition
    );
    log_interactive_rebind_timing(
        timing_enabled,
        timing_sequence,
        "fallback-color-binding-ready",
        timing_started
    );
    PreparedRawFrameDevelopment rebound_development = impl_->development_template.rebind_color(
        effective_plan,
        binding.linear_transform,
        std::move(binding.dcp),
        impl_->development_template.source_scene_luminance_percentile()
    );
    log_interactive_rebind_timing(
        timing_enabled,
        timing_sequence,
        "fallback-development-plan-ready",
        timing_started
    );
    impl_->bind_count.fetch_add(1U, std::memory_order_relaxed);

    if (const auto* ordinary = std::get_if<SensorCfaSourceReconstructionBasis>(&impl_->basis)) {
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
#if SHADOW_IMAGE_HAS_METAL
        if (rebound_development.requested_backend() != RawDevelopmentBackendMode::cpu
            && impl_->ordinary_raw_metal_preview.has_value()) {
            auto resident_attempt = impl_->ordinary_raw_metal_preview->develop(
                ordinary->denoised_frame,
                rebound_development.linear_transform(),
                rebound_development.preview_max_edge(),
                effective_plan.highlight_recovery,
                effective_plan.quality,
                detail::MetalRawDevelopmentContinuations{
                    .dcp_color_transform = dcp,
                }
            );
            if (resident_attempt.development.has_value()
                && (!dcp_requested || resident_attempt.dcp_applied)) {
                developed = std::move(resident_attempt.development);
                fused_dcp_applied = resident_attempt.dcp_applied;
            }
        }
#endif
        log_interactive_rebind_timing(
            timing_enabled,
            timing_sequence,
            "fallback-resident-development-attempted",
            timing_started
        );
        if (!developed.has_value() && dcp_requested
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
        log_interactive_rebind_timing(
            timing_enabled,
            timing_sequence,
            "fallback-secondary-metal-attempted",
            timing_started
        );
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
        log_interactive_rebind_timing(
            timing_enabled,
            timing_sequence,
            developed->backend == RawDevelopmentBackend::metal ? "fallback-development-ready-metal"
                                                               : "fallback-development-ready-cpu",
            timing_started
        );
        impl_->ordinary_raw_bind_count.fetch_add(1U, std::memory_order_relaxed);
        if (developed->backend == RawDevelopmentBackend::metal) {
            impl_->ordinary_raw_metal_development_count.fetch_add(1U, std::memory_order_relaxed);
        } else {
            impl_->ordinary_raw_cpu_development_count.fetch_add(1U, std::memory_order_relaxed);
        }
        DcpColorExecutionBackend dcp_backend =
            fused_dcp_applied ? DcpColorExecutionBackend::metal : DcpColorExecutionBackend::cpu;
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
        log_interactive_rebind_timing(
            timing_enabled,
            timing_sequence,
            dcp_backend == DcpColorExecutionBackend::metal ? "fallback-dcp-ready-metal"
                                                           : "fallback-dcp-ready-cpu",
            timing_started
        );
        RawDevelopmentReceipt receipt = finalize_raw_frame_development_receipt(
            rebound_development,
            developed->scene_linear.dimensions,
            developed->demosaic_receipt,
            developed->backend,
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
        log_interactive_rebind_timing(
            timing_enabled,
            timing_sequence,
            "fallback-receipt-ready",
            timing_started
        );
        return DevelopedSourceReference{
            .source = std::move(developed->scene_linear),
            .raw_development_receipt = std::move(receipt),
            .pipeline_receipt = std::move(pipeline),
            .sensor_clipping_mask = source_reconstruction_sensor_clipping(impl_->basis),
        };
    }

    const auto& foundation = std::get<AiCameraRgbSourceReconstructionBasis>(impl_->basis);
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
        .sensor_clipping_mask = source_reconstruction_sensor_clipping(impl_->basis),
    };
}

PreparedRawPreviewRebinding prepare_raw_preview_rebinding(PreparedRawFrameSource prepared) {
    const RawDevelopmentPlan requested_plan = prepared.pipeline_.requested_plan;
    SensorClippingMask sensor_clipping = project_sensor_clipping_mask(
        prepared.frame_,
        prepared.development_.diagnostic_dimensions()
    );
    RawBayerDenoiseResult conventional = detail::execute_prepared_raw_bayer_denoise(
        std::move(prepared.frame_),
        prepared.development_.raw_denoise()
    );
    SensorCfaSourceReconstructionBasis basis{
        .denoised_frame = std::move(conventional.frame),
        .conventional_denoise = std::move(conventional.receipt),
        .sensor_clipping = std::move(sensor_clipping),
    };
    basis.combined_denoise_identity = basis.conventional_denoise.cache_identity;
#if SHADOW_IMAGE_HAS_METAL
    std::string metal_preview_diagnostic;
    auto metal_preview = detail::MetalRawPreviewRebindingSource::try_prepare(
        basis.denoised_frame,
        metal_preview_diagnostic
    );
#endif
    auto impl = std::make_unique<RawPreviewRebindingSource::Impl>(
        std::move(prepared.development_),
        std::move(prepared.pipeline_),
        requested_plan,
        prepared.plan_negotiation_status_,
        std::move(prepared.metadata_),
        std::move(prepared.camera_profile_definition_),
        SourceReconstructionBasis{std::move(basis)}
#if SHADOW_IMAGE_HAS_METAL
        ,
        std::move(metal_preview)
#endif
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
        SourceReconstructionBasis{
            AiCameraRgbSourceReconstructionBasis{
                .camera_rgb = std::move(camera_rgb),
                .sensor_clipping = std::move(sensor_clipping),
            }
        }
#if SHADOW_IMAGE_HAS_METAL
        ,
        std::nullopt
#endif
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
    const RawDevelopmentPlan effective_plan = source_reconstruction_effective_plan(
        requested_plan,
        SourceReconstructionKind::ai_camera_rgb
    );
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
    const RawDevelopmentPlan effective_plan = source_reconstruction_effective_plan(
        requested_plan,
        SourceReconstructionKind::ai_camera_rgb
    );
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
