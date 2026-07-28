#include "raw_sensor_preparation_test_support.hpp"

namespace {

void raw_denoise_is_cfa_preserving_and_preview_aware() {
    const auto source = noisy_bayer_frame();
    const auto disabled = image::denoise_bayer_raw_frame(
        source,
        image::RawBayerDenoiseRequest{
            .intent = image::RawNoiseReductionIntent::disabled,
            .iso_sensitivity = 6'400.0,
        }
    );
    expect(
        !disabled.receipt.applied() && disabled.frame.samples == source.samples,
        "disabled RAW denoise preserves every sensor sample exactly"
    );

    const auto preview_auto = image::denoise_bayer_raw_frame(
        source,
        image::RawBayerDenoiseRequest{
            .intent = image::RawNoiseReductionIntent::provider_default,
            .iso_sensitivity = 6'400.0,
            .preview = true,
        }
    );
    expect(
        !preview_auto.receipt.applied() && preview_auto.frame.samples == source.samples,
        "automatic RAW denoise keeps bounded previews responsive"
    );

    const auto automatic_detail = image::denoise_bayer_raw_frame(
        source,
        image::RawBayerDenoiseRequest{
            .intent = image::RawNoiseReductionIntent::provider_default,
            .iso_sensitivity = 6'400.0,
        }
    );
    expect(
        automatic_detail.receipt.valid()
            && automatic_detail.receipt.applied()
            && automatic_detail.receipt.effective_intent
                == image::RawNoiseReductionIntent::conservative
            && automatic_detail.receipt.used_sensor_noise_calibration,
        "high-ISO automatic RAW denoise resolves to calibrated conservative CFA processing"
    );
    const auto configured_backend = image::raw_development_backend_mode_from_environment();
    if (configured_backend == image::RawDevelopmentBackendMode::cpu) {
        expect(
            automatic_detail.receipt.backend == image::RawBayerDenoiseBackend::cpu,
            "forced CPU keeps the RAW-denoise receipt on the CPU backend"
        );
    } else if (configured_backend == image::RawDevelopmentBackendMode::metal) {
        expect(
            automatic_detail.receipt.backend == image::RawBayerDenoiseBackend::metal,
            "forced Metal executes high-ISO RAW denoise before demosaic"
        );
    }
    expect(
        flat_cfa_error(automatic_detail.frame) < flat_cfa_error(source),
        "same-CFA RAW denoise reduces flat-field sensor variation before demosaic"
    );
    const auto red = automatic_detail.frame.samples[0U];
    const auto green = automatic_detail.frame.samples[1U];
    const auto blue = automatic_detail.frame.samples[
        static_cast<std::size_t>(automatic_detail.frame.descriptor.storage_dimensions.width) + 1U
    ];
    expect(
        red < 800U && green > 1'000U && blue < 700U,
        "RAW denoise never mixes distinct Bayer colour planes"
    );

    // LibRaw's provider fallback may only advertise provider-default denoise, while the owned
    // RawFrame route can execute a stronger Shadow stage. Preparation must choose its route
    // before negotiating rather than let that fallback capability reject a valid host plan.
    SyntheticRawSession session(source);
    auto robust_preview_plan = image::preview_raw_development_plan();
    robust_preview_plan.noise_reduction = image::RawNoiseReductionIntent::noise_robust;
    const auto warm = image::prepare_warm_edit_preview(
        session,
        4U,
        robust_preview_plan
    );
    expect(
        warm.raw_development_receipt().development_settings_signature.find(
            "raw-denoise=cfa-bilateral-noise-robust-v1"
        ) != std::string::npos
            && session.raw_frame_count() == 1U
            && session.processed_count() == 0U,
        "a host-owned robust RAW plan is not pre-empted by the provider RGB fallback"
    );
    expect(
        warm.sensor_clipping_mask().has_value()
            && warm.sensor_clipping_mask()->valid()
            && warm.sensor_clipping_mask()->dimensions == warm.dimensions(),
        "a warm RAW preview retains its clipping diagnostic without a second provider decode"
    );
}

void raw_denoise_execution_and_calibration_are_cache_visible() {
    const auto source = noisy_bayer_frame();
    const auto first = image::denoise_bayer_raw_frame(
        source,
        image::RawBayerDenoiseRequest{
            .intent = image::RawNoiseReductionIntent::provider_default,
        }
    );
    expect(
        first.receipt.valid()
            && first.receipt.cache_identity.find(
                image::raw_bayer_denoise_backend_identity(first.receipt.backend)
            ) != std::string::npos
            && first.receipt.cache_identity.find(
                "raw-denoise-model=poisson-gaussian-per-cfa-v1"
            ) != std::string::npos,
        "RAW denoise receipt identifies its actual executor and numeric model"
    );

    auto recalibrated = source;
    recalibrated.descriptor.sensor_noise.read_noise_stddev_dn[0] += 1.0;
    const auto second = image::denoise_bayer_raw_frame(
        recalibrated,
        image::RawBayerDenoiseRequest{
            .intent = image::RawNoiseReductionIntent::provider_default,
        }
    );
    expect(
        first.receipt.cache_identity != second.receipt.cache_identity,
        "changing sensor-noise calibration invalidates the RAW denoise identity"
    );

    SyntheticRawSession session(source);
    const auto developed = image::develop_source_reference(
        session,
        image::default_raw_development_plan(),
        std::nullopt,
        image::RawPipelinePolicy{
            .mode = image::RawPipelineMode::require_shadow_raw_frame,
        }
    );
    expect(
        developed.pipeline_receipt.pipeline_identity.find(first.receipt.cache_identity)
                != std::string::npos
            && image::raw_pipeline_receipt_identity(developed.pipeline_receipt).find(
                   first.receipt.cache_identity
               ) != std::string::npos
            && developed.raw_development_receipt.development_settings_signature.find(
                   first.receipt.cache_identity
               ) != std::string::npos,
        "RAW source, canonical cache, and development receipts share denoise provenance"
    );
}

void raw_highlight_treatment_is_executed_and_cache_visible() {
    auto plan = image::default_raw_development_plan();
    plan.highlight_recovery = image::RawHighlightRecoveryIntent::disabled;
    SyntheticRawSession session(synthetic_bayer_frame());
    const auto developed = image::develop_source_reference(
        session,
        plan,
        std::nullopt,
        image::RawPipelinePolicy{
            .mode = image::RawPipelineMode::require_shadow_raw_frame,
        }
    );
    constexpr std::string_view disabled_identity = "sensor-highlights=disabled";
    expect(
        developed.raw_development_receipt.development_settings_signature.find(
            disabled_identity
        ) != std::string::npos
            && developed.pipeline_receipt.pipeline_identity.find(disabled_identity)
                != std::string::npos
            && image::raw_pipeline_receipt_identity(developed.pipeline_receipt).find(
                disabled_identity
            ) != std::string::npos,
        "development, pipeline, and canonical cache identities record actual highlight treatment"
    );
}

void high_quality_raw_plan_is_executed_and_cache_visible() {
    auto plan = image::default_raw_development_plan();
    plan.quality = image::RawDevelopmentQuality::high;
    SyntheticRawSession session(synthetic_bayer_frame());
    const auto developed = image::develop_source_reference(
        session,
        plan,
        std::nullopt,
        image::RawPipelinePolicy{
            .mode = image::RawPipelineMode::require_shadow_raw_frame,
        }
    );
    expect(
        developed.pipeline_receipt.path == image::RawPipelinePath::shadow_raw_frame
            && developed.pipeline_receipt.effective_plan == plan
            && developed.raw_development_receipt.effective_plan == plan,
        "Shadow's owned RawFrame developer accepts the high-quality development plan exactly"
    );
    expect(
        developed.raw_development_receipt.demosaic_quality == 4U
            && developed.raw_development_receipt.development_settings_signature.find(
                "demosaic=bayer-edge-aware"
            ) != std::string::npos
            && image::raw_pipeline_receipt_identity(developed.pipeline_receipt).find(
                "quality=high"
            ) != std::string::npos,
        "high-quality reconstruction is visible in source and cache provenance"
    );
}

void raw_frame_source_calibration_is_identical_for_preview_and_detail() {
    SyntheticRawSession session(gradient_bayer_frame());
    const auto preview = image::develop_source_reference(
        session,
        image::preview_raw_development_plan(),
        2U,
        image::RawPipelinePolicy{.mode = image::RawPipelineMode::require_shadow_raw_frame}
    );
    const auto detail = image::develop_source_reference(
        session,
        image::default_raw_development_plan(),
        std::nullopt,
        image::RawPipelinePolicy{.mode = image::RawPipelineMode::require_shadow_raw_frame}
    );
    expect(
        preview.pipeline_receipt.source_scene_luminance_percentile.has_value()
            && detail.pipeline_receipt.source_scene_luminance_percentile.has_value()
            && std::abs(
                *preview.pipeline_receipt.source_scene_luminance_percentile
                - *detail.pipeline_receipt.source_scene_luminance_percentile
            ) < 1.0e-12,
        "owned RAW source calibration is measured before the preview/detail render split"
    );
    const auto preview_receipt = image::resolve_source_rendering(
        std::get<image::SceneLinearRgbFrame>(preview.source),
        session.metadata(),
        preview.pipeline_receipt
    );
    const auto detail_receipt = image::resolve_source_rendering(
        std::get<image::SceneLinearRgbFrame>(detail.source),
        session.metadata(),
        detail.pipeline_receipt
    );
    expect(
        std::abs(
            preview_receipt.standard_exposure_normalization_stops
            - detail_receipt.standard_exposure_normalization_stops
        ) < 1.0e-12,
        "warm preview and full detail use one identical Shadow Standard exposure calibration"
    );
    expect(
        image::raw_pipeline_receipt_identity(preview.pipeline_receipt).find(
            "source-luminance-p99="
        ) != std::string::npos,
        "stable RAW source calibration participates in pipeline cache provenance"
    );
}
} // namespace

int main() {
    raw_denoise_is_cfa_preserving_and_preview_aware();
    raw_denoise_execution_and_calibration_are_cache_visible();
    raw_highlight_treatment_is_executed_and_cache_visible();
    high_quality_raw_plan_is_executed_and_cache_visible();
    raw_frame_source_calibration_is_identical_for_preview_and_detail();
    return failures == 0 ? 0 : 1;
}
