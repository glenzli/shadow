#include "raw_pipeline_contract_test_support.hpp"
#include "raw_pipeline_routing_test_support.hpp"
#include "scoped_environment.hpp"

#include "../src/raw/raw_frame_source_preparation.hpp"
#include "../src/raw/raw_preview_rebinding.hpp"

#include <shadow/image/raw_foundation.hpp>

#include <array>
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

[[nodiscard]] image::RawDevelopmentPlan manual_white_balance_plan() {
    auto plan = image::preview_raw_development_plan();
    plan.white_balance = {
        .mode = image::RawWhiteBalanceMode::temperature_tint,
        .temperature_kelvin = 6'800U,
        .tint = 18,
    };
    return plan;
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

    const auto rebound = initial.rebind_raw_development_plan(manual_white_balance_plan());
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

} // namespace

int main() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    ordinary_raw_rebinds_without_a_second_decode();
    decoder_matrix_dcp_rebind_keeps_initial_color_stage_policy();
    ai_foundation_rebinds_its_bounded_camera_rgb_without_a_second_decode();
    return failures == 0 ? 0 : 1;
}
