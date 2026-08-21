#include "shadow/image/dcp_color_development.hpp"

#include "raw_pipeline_routing_test_support.hpp"

namespace {

void automatic_pipeline_prefers_owned_raw_frame() {
    SyntheticRawSession session(synthetic_bayer_frame());
    const auto developed = image::develop_source_reference(
        session,
        image::preview_raw_development_plan(),
        2U
    );
    expect(
        developed.pipeline_receipt.path == image::RawPipelinePath::shadow_raw_frame
            && developed.pipeline_receipt.raw_developer_version
                == image::shadow_raw_frame_developer_version,
        "automatic RAW preparation records Shadow's owned RawFrame developer"
    );
    expect(
        std::holds_alternative<image::SceneLinearRgbFrame>(developed.source)
            && std::get<image::SceneLinearRgbFrame>(developed.source).dimensions
                == image::Dimensions{2U, 2U},
        "area preview produces bounded standardized scene-linear RGB"
    );
    expect(
        session.raw_frame_count() == 1U && session.processed_count() == 0U,
        "supported RawFrame preparation never asks the provider to develop RGB"
    );
    expect(
        developed.sensor_clipping_mask.has_value()
            && developed.sensor_clipping_mask->valid()
            && developed.sensor_clipping_mask->dimensions == image::Dimensions{2U, 2U},
        "RAW development carries a valid source clipping map at the same bounded display extent"
    );
    expect(
        developed.raw_development_receipt.development_settings_signature.find(
            "bayer-area-preview"
        ) != std::string::npos,
        "preview receipt distinguishes CFA area integration from full bilinear development"
    );
    for (std::size_t pixel = 0U; pixel < 4U; ++pixel) {
        const std::size_t index = pixel * 3U;
        const auto& pixels = std::get<image::SceneLinearRgbFrame>(developed.source).samples;
        const auto red = pixels[index];
        const auto green = pixels[index + 1U];
        const auto blue = pixels[index + 2U];
        expect(
            red == green && green == blue && red >= 0.199F && red <= 0.201F,
            "as-shot neutral and camera matrix are applied after CFA-aware area integration"
        );
    }
}

void full_pipeline_records_the_effective_backend_in_every_identity() {
    const auto configured_mode = image::raw_development_backend_mode_from_environment();
    const auto expected_backend = configured_mode == image::RawDevelopmentBackendMode::cpu
        ? image::RawDevelopmentBackend::cpu
        : configured_mode == image::RawDevelopmentBackendMode::metal
            ? image::RawDevelopmentBackend::metal
            : image::raw_development_backend_available(image::RawDevelopmentBackend::metal)
                ? image::RawDevelopmentBackend::metal
                : image::RawDevelopmentBackend::cpu;
    const auto expected_identity = image::raw_development_backend_identity(expected_backend);
    SyntheticRawSession session(synthetic_bayer_frame());
    const auto developed = image::develop_source_reference(
        session,
        image::default_raw_development_plan(),
        std::nullopt,
        image::RawPipelinePolicy{
            .mode = image::RawPipelineMode::require_shadow_raw_frame,
        }
    );
    expect(
        developed.raw_development_receipt.development_settings_signature.find(
            expected_identity
        ) != std::string::npos,
        "full development receipt records the effective CPU or Metal backend"
    );
    expect(
        developed.pipeline_receipt.pipeline_identity.find(expected_identity)
            != std::string::npos,
        "pipeline receipt records the same effective backend"
    );
    expect(
        image::raw_pipeline_receipt_identity(developed.pipeline_receipt).find(
            expected_identity
        ) != std::string::npos,
        "canonical pipeline cache identity retains the effective backend"
    );
}

void unsupported_host_stage_falls_back_explicitly() {
    SyntheticRawSession session(synthetic_bayer_frame(false));
    const auto developed = image::develop_source_reference(
        session,
        image::preview_raw_development_plan(),
        2U
    );
    expect(
        developed.pipeline_receipt.path
                == image::RawPipelinePath::provider_processed_compatibility
            && !developed.pipeline_receipt.fallback_reason.empty(),
        "automatic mode records why an unsupported host developer used provider RGB"
    );
    expect(
        session.raw_frame_count() == 1U && session.processed_count() == 1U
            && std::get<image::PixelBuffer>(developed.source).samples.front() == 7'777U,
        "fallback reuses the same provider session without hiding its compatibility pixels"
    );

    SyntheticRawSession required_session(synthetic_bayer_frame(false));
    try {
        static_cast<void>(image::develop_source_reference(
            required_session,
            image::preview_raw_development_plan(),
            2U,
            image::RawPipelinePolicy{
                .mode = image::RawPipelineMode::require_shadow_raw_frame,
            }
        ));
        expect(false, "required RawFrame mode rejects missing camera calibration");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported,
            "required RawFrame mode returns a typed unsupported error"
        );
    }
}

void exact_dcp_replaces_missing_generic_matrix() {
    SyntheticRawSession session(
        synthetic_bayer_frame(false),
        "Open Camera",
        "Mk I"
    );
    const auto developed = image::develop_source_reference(
        session,
        image::preview_raw_development_plan(),
        2U,
        image::RawPipelinePolicy{
            .mode = image::RawPipelineMode::require_shadow_raw_frame,
        },
        exact_dcp_catalog()
    );
    expect(
        developed.pipeline_receipt.path == image::RawPipelinePath::shadow_raw_frame
            && developed.pipeline_receipt.camera_profile_status
                == image::RawCameraProfileStatus::applied,
        "exact DCP provides color calibration when provider generic matrix is absent"
    );
    expect(
        developed.pipeline_receipt.camera_profile_identity
                == "sha256:synthetic-open-camera-dcp"
            && developed.pipeline_receipt.camera_profile_developer_version
                == image::dcp_color_developer_version,
        "DCP content and developer identities are cache-visible"
    );
    expect(
        session.raw_frame_count() == 1U && session.processed_count() == 0U,
        "DCP-calibrated RawFrame never asks provider for processed RGB"
    );
    expect(
        std::holds_alternative<image::SceneLinearRgbFrame>(developed.source),
        "DCP post stages keep owned RAW development at the scene-linear fp32 boundary"
    );
    if (!std::holds_alternative<image::SceneLinearRgbFrame>(developed.source)) {
        return;
    }
    const auto& developed_pixels = std::get<image::SceneLinearRgbFrame>(developed.source).samples;
    expect(
        *std::max_element(developed_pixels.begin(), developed_pixels.end()) > 1.0F,
        "DCP post stages preserve super-white RAW values for later highlight recovery"
    );
    for (std::size_t pixel = 0U; pixel < 4U; ++pixel) {
        const std::size_t index = pixel * 3U;
        expect(
            developed_pixels[index] > 0.0F
                && std::abs(
                    developed_pixels[index + 1U] - developed_pixels[index]
                ) < 1.0e-3F
                && std::abs(
                    developed_pixels[index + 2U] - developed_pixels[index]
                ) < 1.0e-3F,
            "DCP ForwardMatrix keeps the neutral and applies BaselineExposureOffset"
        );
    }
}

void host_raw_frame_capabilities_are_not_limited_by_provider_rgb_fallbacks() {
    const auto capabilities = image::shadow_raw_frame_development_capabilities();
    expect(
        capabilities.available && capabilities.raw_frame
            && (capabilities.supported_qualities
                & image::raw_development_quality_mask(image::RawDevelopmentQuality::high)) != 0U
            && (capabilities.supported_noise_reduction_intents
                & image::raw_noise_reduction_intent_mask(
                    image::RawNoiseReductionIntent::noise_robust
                )) != 0U,
        "host RawFrame capabilities declare the high-quality and RAW-denoise stages Shadow owns"
    );

    auto host_plan = image::preview_raw_development_plan();
    host_plan.quality = image::RawDevelopmentQuality::high;
    host_plan.noise_reduction = image::RawNoiseReductionIntent::noise_robust;
    const auto accepted = image::negotiate_shadow_raw_frame_development_plan(host_plan);
    expect(
        accepted.accepted() && accepted.effective == host_plan,
        "host RawFrame negotiation accepts a plan even when a provider RGB fallback is conservative"
    );

    host_plan.highlight_recovery = image::RawHighlightRecoveryIntent::aggressive;
    const auto aggressive_accepted = image::negotiate_shadow_raw_frame_development_plan(host_plan);
    expect(
        aggressive_accepted.accepted() && aggressive_accepted.effective == host_plan,
        "host RawFrame negotiation exposes the opt-in destructive highlight repair stage explicitly"
    );
}

} // namespace

int main() {
    automatic_pipeline_prefers_owned_raw_frame();
    full_pipeline_records_the_effective_backend_in_every_identity();
    unsupported_host_stage_falls_back_explicitly();
    exact_dcp_replaces_missing_generic_matrix();
    host_raw_frame_capabilities_are_not_limited_by_provider_rgb_fallbacks();
    return failures == 0 ? 0 : 1;
}
