#include "raw_pipeline_contract_test_support.hpp"
#include "raw_pipeline_routing_test_support.hpp"
#include "scoped_environment.hpp"

#include "../src/raw/metal_raw_development.hpp"
#include "../src/raw/raw_frame_source_preparation.hpp"
#include "../src/raw/raw_preview_rebinding.hpp"
#include "../src/raw/raw_source_reconstruction.hpp"

#include <shadow/image/edit_error.hpp>
#include <shadow/image/raw_foundation.hpp>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using shadow::image::test_support::ScopedEnvironment;

constexpr std::string_view source_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view artifact_digest =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view cache_key_digest =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

[[nodiscard]] bool metal_is_required() {
    return std::getenv("SHADOW_TEST_REQUIRE_METAL") != nullptr;
}

[[nodiscard]] image::RawDevelopmentPlan manual_white_balance_plan() {
    auto plan = image::preview_raw_development_plan();
    plan.white_balance = {
        .mode = image::RawWhiteBalanceMode::temperature_tint,
        .temperature_kelvin = 6'800U,
        .tint = 18,
    };
    return plan;
}

[[nodiscard]] std::array<image::AdjustmentLayer, 1U> composite_layers() {
    image::LocalMask composite;
    composite.components = {
        image::LocalMaskComponent{
            .operation = image::LocalMaskComponentOperation::base,
            .mask =
                image::LocalMask{
                    .kind = image::LocalMaskKind::linear_gradient,
                    .x0 = 0.1,
                    .y0 = 0.2,
                    .x1 = 0.9,
                    .y1 = 0.8,
                },
        },
        image::LocalMaskComponent{
            .operation = image::LocalMaskComponentOperation::add,
            .mask = image::LocalMask{
                .kind = image::LocalMaskKind::radial_gradient,
                .x0 = 0.5,
                .y0 = 0.5,
                .radius_x = 0.3,
                .radius_y = 0.25,
                .feather = 0.5,
            },
        },
    };
    return {
        image::AdjustmentLayer{
            .layer_id = "resident-raw-composite",
            .mask = std::move(composite),
            .nodes = {
                image::AdjustmentNode{
                    .node_id = "resident-raw-composite-exposure",
                    .parameters = image::ExposureAdjustment{.stops = 0.24},
                },
            },
        },
    };
}

[[nodiscard]] image::RawFoundationCameraRgbView foundation(const std::vector<float>& pixels) {
    return image::RawFoundationCameraRgbView{
        .dimensions = {4U, 4U},
        .samples = pixels,
        .provenance = {
            .source_sha256 = std::string(source_digest),
            .artifact_file_sha256 = std::string(artifact_digest),
            .cache_key_sha256 = std::string(cache_key_digest),
            .model_identity = std::string(image::raw_foundation_model_identity),
            .implementation_revision = std::string(image::raw_foundation_implementation_revision),
        },
    };
}

void ordinary_raw_rebinds_without_a_second_decode() {
    SyntheticRawSession decoder(synthetic_bayer_frame());
    const auto initial =
        image::prepare_warm_edit_preview(decoder, 4U, image::preview_raw_development_plan());
    expect(
        initial.supports_raw_development_rebinding(),
        "owned RawFrame warm preview advertises camera-space colour rebinding"
    );
    const auto automatic_white_balance = initial.auto_raw_white_balance();
    expect(
        initial.supports_raw_white_balance_picker() && automatic_white_balance.has_value()
            && automatic_white_balance->valid() && decoder.raw_frame_count() == 1U,
        "automatic RAW white balance samples the retained CFA without a second decode"
    );

    const auto rebound = initial.rebind_raw_development_plan(manual_white_balance_plan());
    expect(
        initial.highlight_chroma_risk_map().has_value()
            && rebound.highlight_chroma_risk_map().has_value()
            && initial.highlight_chroma_risk_map()->samples
                   == rebound.highlight_chroma_risk_map()->samples
            && initial.highlight_chroma_risk_map()->source_surface_reconstructed
            && rebound.highlight_chroma_risk_map()->source_surface_reconstructed,
        "white balance preserves exact immutable completed highlight evidence"
    );
    const auto telemetry = rebound.raw_rebinding_telemetry();
    expect(
        decoder.raw_frame_count() == 1U && decoder.processed_count() == 0U,
        "ordinary RAW white-balance rebinding neither decodes nor enters provider RGB"
    );
    expect(
        telemetry.bind_count == 2U && telemetry.ordinary_raw_bind_count == 2U
            && telemetry.ordinary_raw_cpu_development_count == 2U
            && telemetry.ordinary_raw_metal_development_count == 0U
            && telemetry.ordinary_raw_fused_dcp_bind_count == 0U,
        "ordinary RAW rebind telemetry records the retained source and its actual backend"
    );
    expect(
        rebound.raw_development_receipt().requested_plan.white_balance
                == manual_white_balance_plan().white_balance
            && initial.raw_development_receipt().requested_plan.white_balance.mode
                   == image::RawWhiteBalanceMode::as_shot,
        "rebound and original immutable sessions retain independent exact RAW receipts"
    );
    expect(
        initial.raw_pipeline_receipt().pipeline_identity.find(
            "clipped-highlight=cfa-opposed-physical-white-chrominance-v24"
        ) != std::string::npos
            && rebound.raw_pipeline_receipt().pipeline_identity.find(
                   "clipped-highlight=cfa-opposed-physical-white-chrominance-v24"
               ) != std::string::npos,
        "warm white-balance rebinding retains the cache-visible physical-white source policy"
    );

    const std::array<image::AdjustmentNode, 0U> neutral{};
    const auto original_pixels = initial.render_rgb8(neutral);
    const auto rebound_pixels = rebound.render_rgb8(neutral);
    expect(
        original_pixels.bytes != rebound_pixels.bytes,
        "camera-domain white-balance rebinding changes the rendered preview"
    );

    auto incompatible = manual_white_balance_plan();
    incompatible.noise_reduction = image::RawNoiseReductionIntent::noise_robust;
    try {
        static_cast<void>(initial.rebind_raw_development_plan(incompatible));
        expect(false, "rebinding must reject a sensor-stage plan change");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "sensor-stage changes fail before a camera-space rebind"
        );
    }
}

void decoder_matrix_dcp_rebind_keeps_initial_color_stage_policy() {
    auto frame = synthetic_bayer_frame();
    SyntheticRawSession decoder(std::move(frame), "Open Camera", "Mk I");
    const auto catalog = exact_dcp_catalog();
    auto prepared = image::raw_pipeline_detail::prepare_raw_frame_source(
        decoder,
        image::preview_raw_development_plan(),
        4U,
        catalog
    );
    auto rebound = image::raw_pipeline_detail::prepare_raw_preview_rebinding(std::move(prepared));
    const auto manual = rebound.source->bind(manual_white_balance_plan());

    expect(
        manual.raw_development_receipt.development_settings_signature.find(";huesat=none")
                != std::string::npos
            && manual.raw_development_receipt.development_settings_signature.find(";look=none")
                   != std::string::npos
            && manual.raw_development_receipt.development_settings_signature.find(";tone=none")
                   != std::string::npos,
        "decoder-matrix DCP rebind keeps post-matrix DCP stages disabled like initial preparation"
    );
    expect(
        rebound.source->telemetry().ordinary_raw_fused_dcp_bind_count == 0U
            && rebound.source->telemetry().dcp_cpu_execution_count == 0U
            && rebound.source->telemetry().dcp_metal_execution_count == 0U,
        "decoder-matrix DCP rebind does not reintroduce a separate post-matrix DCP render"
    );
}

void ordinary_raw_rebind_uses_the_retained_metal_source() {
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        if (metal_is_required()) {
            expect(
                false,
                "the required Metal RAW preview rebinding backend is available: "
                    + image::detail::metal_raw_development_diagnostic()
            );
        }
        return;
    }
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "metal");
    SyntheticRawSession decoder(synthetic_bayer_frame());
    const auto initial =
        image::prepare_warm_edit_preview(decoder, 4U, image::preview_raw_development_plan());
    const auto rebound = initial.rebind_raw_development_plan(manual_white_balance_plan());
    const auto telemetry = rebound.raw_rebinding_telemetry();
    expect(
        decoder.raw_frame_count() == 1U && decoder.processed_count() == 0U,
        "Metal RAW rebinding retains the decoded CFA source rather than reopening the decoder"
    );
    expect(
        telemetry.bind_count == 2U && telemetry.ordinary_raw_metal_development_count == 2U
            && telemetry.ordinary_raw_cpu_development_count == 0U,
        "Metal RAW rebinding completes both source preparations on the GPU path"
    );
    expect(
        rebound.gpu_stats().resident && rebound.gpu_stats().source_upload_count == 0U,
        "Metal RAW rebinding adopts the freshly developed device buffer without a host upload"
    );

    const auto layers = composite_layers();
    {
        const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
        const auto first = rebound.render_jpeg_with_analysis_layers(layers, 95U);
        const auto repeated = rebound.render_jpeg_with_analysis_layers(layers, 95U);
        const auto coverage =
            rebound.render_jpeg_with_analysis_layers_and_mask_coverage_cancellable(
                layers,
                0U,
                95U,
                {}
            );
        expect(
            !first.proxy.bytes.empty() && first.proxy.bytes == repeated.proxy.bytes
                && first.execution.adjustment_backend == image::EditPreviewBackend::cpu
                && first.execution.display_backend == image::EditPreviewBackend::cpu
                && first.execution.adjustment_fell_back && first.execution.display_fell_back
                && first.execution.diagnostic.find("exact CPU replay") != std::string::npos
                && coverage.completed.has_value() && coverage.completed->mask_coverage.has_value()
                && coverage.completed->mask_coverage->valid()
                && coverage.completed->preview.execution.adjustment_backend
                       == image::EditPreviewBackend::cpu
                && decoder.raw_frame_count() == 1U
                && rebound.raw_rebinding_telemetry().bind_count == 2U,
            "automatic composite preview and coverage replay one cached host copy of resident RAW"
        );
    }
    try {
        static_cast<void>(rebound.render_jpeg_with_analysis_layers(layers, 95U));
        expect(false, "forced Metal composite preview must fail before CPU replay");
    } catch (const image::EditError& error) {
        expect(
            error.code() == image::EditErrorCode::backend_failure
                && decoder.raw_frame_count() == 1U,
            "forced Metal composite preview remains a typed fail-closed route"
        );
    }
}

void retained_metal_cfa_preview_matches_the_one_shot_kernel() {
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        if (metal_is_required()) {
            expect(
                false,
                "the required Metal RAW preview rebinding backend is available: "
                    + image::detail::metal_raw_development_diagnostic()
            );
        }
        return;
    }
    const auto frame = synthetic_bayer_frame();
    std::string diagnostic;
    auto retained = image::detail::MetalRawPreviewRebindingSource::try_prepare(frame, diagnostic);
    expect(
        retained.has_value(),
        "Metal preview rebinding retains one immutable CFA sensor plane when Metal is available"
    );
    if (!retained.has_value()) {
        return;
    }
    const image::RawFrameLinearTransform transform{{
        1.13,
        -0.08,
        0.02,
        -0.04,
        1.06,
        -0.01,
        0.03,
        -0.11,
        1.19,
    }};
    const auto one_shot = image::detail::try_develop_bayer_linear_srgb_f32_metal(
        frame,
        transform,
        2U,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::balanced
    );
    const auto rebound = retained->develop(
        frame,
        transform,
        2U,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::balanced
    );
    expect(
        one_shot.development.has_value() && rebound.development.has_value(),
        "one-shot and retained Metal CFA preview routes both complete"
    );
    if (!one_shot.development.has_value() || !rebound.development.has_value()) {
        return;
    }
    expect(
        one_shot.development->scene_linear.dimensions
                == rebound.development->scene_linear.dimensions
            && one_shot.development->scene_linear.samples
                   == rebound.development->scene_linear.samples
            && one_shot.development->demosaic_receipt.algorithm
                   == rebound.development->demosaic_receipt.algorithm
            && one_shot.development->demosaic_receipt.white_balance_applied
                   == rebound.development->demosaic_receipt.white_balance_applied,
        "retained CFA preview uses the exact one-shot reconstruction kernel and receipt"
    );
}

void ai_foundation_rebinds_its_bounded_camera_rgb_without_a_second_decode() {
    const std::vector<float> pixels = [] {
        std::vector<float> values(4U * 4U * 3U);
        for (std::size_t index = 0U; index < values.size(); index += 3U) {
            values[index] = 0.125F;
            values[index + 1U] = 0.25F;
            values[index + 2U] = 0.5F;
        }
        return values;
    }();
    SyntheticRawSession decoder(synthetic_bayer_frame());
    const auto initial = image::prepare_warm_edit_preview(
        decoder,
        2U,
        image::preview_raw_development_plan(),
        foundation(pixels)
    );
    const auto rebound = initial.rebind_raw_development_plan(manual_white_balance_plan());
    const auto amount_rebound =
        initial.rebind_raw_foundation_amount(image::preview_raw_development_plan(), 25U);
    const auto telemetry = amount_rebound.raw_rebinding_telemetry();
    expect(
        initial.supports_raw_development_rebinding() && decoder.raw_frame_count() == 1U
            && decoder.processed_count() == 0U,
        "AI RAW foundation rebind shares its bounded camera RGB and source calibration"
    );
    expect(
        !initial.supports_raw_white_balance_picker()
            && !initial.auto_raw_white_balance().has_value(),
        "automatic RAW white balance fails closed for a camera-RGB foundation source"
    );
    expect(
        initial.supports_raw_foundation_amount_rebinding() && decoder.raw_frame_count() == 1U,
        "AI amount rebind retains paired bounded bases without a second RAW decode"
    );
    expect(
        telemetry.bind_count == 3U && telemetry.foundation_camera_rgb_bind_count == 3U
            && telemetry.foundation_amount_bind_count == 1U
            && telemetry.ordinary_raw_bind_count == 0U,
        "AI RAW rebind telemetry distinguishes camera-RGB and amount-only source work"
    );
    expect(
        rebound.raw_pipeline_receipt().pipeline_identity.find(artifact_digest) != std::string::npos
            && rebound.raw_development_receipt().effective_plan.noise_reduction
                   == image::RawNoiseReductionIntent::disabled
            && rebound.raw_development_receipt().effective_plan.white_balance
                   == manual_white_balance_plan().white_balance,
        "AI rebound retains foundation identity and its adjusted sensor-stage provenance"
    );

    const std::array<image::AdjustmentNode, 0U> neutral{};
    expect(
        initial.render_rgb8(neutral).bytes != rebound.render_rgb8(neutral).bytes,
        "AI camera-RGB foundation receives the new white-balanced camera-domain colour binding"
    );
    expect(
        initial.render_rgb8(neutral).bytes != amount_rebound.render_rgb8(neutral).bytes
            && amount_rebound.raw_pipeline_receipt().pipeline_identity.find("amount-percent=25")
                   != std::string::npos,
        "AI amount rebind changes pixels and publishes the exact developed amount identity"
    );
}

void source_reconstruction_basis_names_retained_sensor_capability() {
    image::raw_pipeline_detail::SensorCfaSourceReconstructionBasis sensor{
        .denoised_frame = synthetic_bayer_frame(),
        .sensor_clipping = {
            .dimensions = {.width = 1U, .height = 1U},
            .samples = {image::sensor_highlight_clipped},
            .highlight_pixel_count = 1U,
        },
    };
    image::raw_pipeline_detail::AiCameraRgbSourceReconstructionBasis ai{
        .camera_rgb =
            {
                .dimensions = {.width = 1U, .height = 1U},
                .row_stride_bytes = 3U * sizeof(float),
                .samples = {0.25F, 0.5F, 0.75F},
                .source_camera_rgb_dimensions = {.width = 1U, .height = 1U},
                .foundation_cache_identity = "test-foundation",
            },
        .sensor_clipping = {
            .dimensions = {.width = 1U, .height = 1U},
            .samples = {image::sensor_highlight_clipped},
            .highlight_pixel_count = 1U,
        },
    };
    const image::raw_pipeline_detail::SourceReconstructionBasis sensor_basis{std::move(sensor)};
    const image::raw_pipeline_detail::SourceReconstructionBasis ai_basis{std::move(ai)};
    expect(
        image::raw_pipeline_detail::source_reconstruction_retains_sensor_cfa(sensor_basis)
            && !image::raw_pipeline_detail::source_reconstruction_retains_sensor_cfa(ai_basis)
            && image::raw_pipeline_detail::source_reconstruction_sensor_clipping(sensor_basis)
                   .valid()
            && image::raw_pipeline_detail::source_reconstruction_sensor_clipping(ai_basis).valid(),
        "the common source-reconstruction basis exposes one immutable sensor-evidence contract"
    );
}

} // namespace

int main() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    ordinary_raw_rebinds_without_a_second_decode();
    decoder_matrix_dcp_rebind_keeps_initial_color_stage_policy();
    ordinary_raw_rebind_uses_the_retained_metal_source();
    retained_metal_cfa_preview_matches_the_one_shot_kernel();
    ai_foundation_rebinds_its_bounded_camera_rgb_without_a_second_decode();
    source_reconstruction_basis_names_retained_sensor_capability();
    return failures == 0 ? 0 : 1;
}
