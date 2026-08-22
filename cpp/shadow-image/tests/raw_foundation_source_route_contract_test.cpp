#include "raw_pipeline_routing_test_support.hpp"

#include "../src/raw/raw_source_reconstruction.hpp"

#include <shadow/image/raw_foundation.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr std::string_view source_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view artifact_digest =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view cache_key_digest =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

[[nodiscard]] image::RawFoundationCameraRgbView foundation(
    const std::vector<float>& pixels,
    const image::Dimensions dimensions = {.width = 4U, .height = 4U}
) {
    return image::RawFoundationCameraRgbView{
        .dimensions = dimensions,
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

void verified_foundation_is_the_only_reconstruction_source() {
    SyntheticRawSession session(synthetic_bayer_frame());
    std::vector<float> pixels(4U * 4U * 3U);
    for (std::size_t index = 0U; index < pixels.size(); index += 3U) {
        pixels[index] = 0.125F;
        pixels[index + 1U] = 0.25F;
        pixels[index + 2U] = 0.5F;
    }

    image::RawDevelopmentPlan requested = image::preview_raw_development_plan();
    requested.noise_reduction = image::RawNoiseReductionIntent::noise_robust;
    requested.highlight_recovery = image::RawHighlightRecoveryIntent::provider_default;
    const auto developed = image::develop_source_reference(
        session,
        requested,
        foundation(pixels),
        2U,
        image::RawPipelinePolicy{
            .mode = image::RawPipelineMode::require_shadow_raw_frame,
        }
    );

    expect(
        session.raw_frame_count() == 1U && session.processed_count() == 0U,
        "foundation route decodes source calibration once and never asks for provider RGB"
    );
    expect(
        std::holds_alternative<image::SceneLinearRgbFrame>(developed.source)
            && std::get<image::SceneLinearRgbFrame>(developed.source).dimensions
                   == image::Dimensions{.width = 2U, .height = 2U},
        "verified foundation produces the bounded scene-linear preview"
    );
    expect(
        developed.pipeline_receipt.path == image::RawPipelinePath::shadow_raw_frame
            && developed.pipeline_receipt.pipeline_identity.find("raw-foundation-developer")
                   != std::string::npos
            && developed.pipeline_receipt.pipeline_identity.find(artifact_digest)
                   != std::string::npos,
        "pipeline provenance names the AI source route and verified artifact digest"
    );
    expect(
        developed.raw_development_receipt.development_settings_signature.find(
            "reconstruction=rawnind-public-bayer"
        ) != std::string::npos
            && developed.raw_development_receipt.development_settings_signature.find(
                   "demosaic=bayer"
               ) == std::string::npos,
        "RAW receipt never represents AI reconstruction as a conventional Bayer demosaic"
    );
    expect(
        developed.raw_development_receipt.requested_plan == requested
            && developed.raw_development_receipt.effective_plan.noise_reduction
                   == image::RawNoiseReductionIntent::disabled
            && developed.raw_development_receipt.effective_plan.highlight_recovery
                   == image::RawHighlightRecoveryIntent::disabled
            && developed.raw_development_receipt.plan_negotiation_status
                   == image::RawDevelopmentPlanNegotiationStatus::adjusted,
        "AI execution preserves requested intent and audits the disabled overlapping RAW stages"
    );
    expect(
        developed.sensor_clipping_mask.has_value() && developed.sensor_clipping_mask->valid()
            && developed.sensor_clipping_mask->dimensions
                   == image::Dimensions{.width = 2U, .height = 2U},
        "common AI source basis carries sensor clipping evidence aligned with the preview"
    );
}

void artifact_mismatch_and_processed_policy_fail_closed() {
    const std::vector<float> valid_pixels(4U * 4U * 3U, 0.25F);
    SyntheticRawSession wrong_geometry_session(synthetic_bayer_frame());
    const std::vector<float> wrong_pixels(2U * 4U * 3U, 0.25F);
    try {
        static_cast<void>(image::develop_source_reference(
            wrong_geometry_session,
            image::preview_raw_development_plan(),
            foundation(wrong_pixels, image::Dimensions{.width = 2U, .height = 4U}),
            2U
        ));
        expect(false, "geometry mismatch must fail");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request
                && wrong_geometry_session.processed_count() == 0U,
            "geometry mismatch cannot silently substitute the original RAW or provider RGB"
        );
    }

    SyntheticRawSession processed_session(synthetic_bayer_frame());
    try {
        static_cast<void>(image::develop_source_reference(
            processed_session,
            image::preview_raw_development_plan(),
            foundation(valid_pixels),
            2U,
            image::RawPipelinePolicy{
                .mode = image::RawPipelineMode::require_provider_processed,
            }
        ));
        expect(false, "processed compatibility policy must reject a foundation");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported
                && processed_session.raw_frame_count() == 0U
                && processed_session.processed_count() == 0U,
            "policy conflict fails before either source route is decoded"
        );
    }
}

void artifact_identity_changes_the_canonical_pipeline_identity() {
    const std::vector<float> pixels(4U * 4U * 3U, 0.25F);
    const image::CameraProfileCatalog catalog = exact_dcp_catalog();
    SyntheticRawSession first_session(synthetic_bayer_frame());
    const auto first = image::develop_source_reference(
        first_session,
        image::preview_raw_development_plan(),
        foundation(pixels),
        2U,
        image::default_raw_pipeline_policy(),
        catalog
    );

    auto second_foundation = foundation(pixels);
    second_foundation.provenance.artifact_file_sha256 =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    SyntheticRawSession second_session(synthetic_bayer_frame());
    const auto second = image::develop_source_reference(
        second_session,
        image::preview_raw_development_plan(),
        second_foundation,
        2U,
        image::default_raw_pipeline_policy(),
        catalog
    );
    expect(
        image::raw_pipeline_receipt_identity(first.pipeline_receipt)
            != image::raw_pipeline_receipt_identity(second.pipeline_receipt),
        "verified artifact digest participates in the canonical source cache identity"
    );
}

void source_reconstruction_policy_preserves_sensor_cfa_and_disables_overlapping_ai_stages() {
    image::RawDevelopmentPlan requested = image::preview_raw_development_plan();
    requested.noise_reduction = image::RawNoiseReductionIntent::noise_robust;
    requested.highlight_recovery = image::RawHighlightRecoveryIntent::aggressive;

    const auto sensor = image::raw_pipeline_detail::source_reconstruction_effective_plan(
        requested,
        image::raw_pipeline_detail::SourceReconstructionKind::sensor_cfa
    );
    const auto ai = image::raw_pipeline_detail::source_reconstruction_effective_plan(
        requested,
        image::raw_pipeline_detail::SourceReconstructionKind::ai_camera_rgb
    );
    expect(
        sensor == requested
            && image::raw_pipeline_detail::source_reconstruction_retains_sensor_cfa(
                image::raw_pipeline_detail::SourceReconstructionKind::sensor_cfa
            ),
        "sensor-CFA source reconstruction keeps its authored RAW development stages"
    );
    expect(
        ai.noise_reduction == image::RawNoiseReductionIntent::disabled
            && ai.highlight_recovery == image::RawHighlightRecoveryIntent::disabled
            && !image::raw_pipeline_detail::source_reconstruction_retains_sensor_cfa(
                image::raw_pipeline_detail::SourceReconstructionKind::ai_camera_rgb
            ),
        "AI camera-RGB source reconstruction disables only Bayer-only stages"
    );
}

} // namespace

int main() {
    verified_foundation_is_the_only_reconstruction_source();
    artifact_mismatch_and_processed_policy_fail_closed();
    artifact_identity_changes_the_canonical_pipeline_identity();
    source_reconstruction_policy_preserves_sensor_cfa_and_disables_overlapping_ai_stages();
    return failures == 0 ? 0 : 1;
}
