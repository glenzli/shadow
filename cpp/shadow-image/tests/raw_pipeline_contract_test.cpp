#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/edit.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_denoise.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] image::RawDevelopmentCapabilities raw_capabilities() {
    auto capabilities = image::RawDevelopmentCapabilities{};
    capabilities.schema_version = image::raw_development_capabilities_schema_version;
    capabilities.available = true;
    capabilities.raw_frame = true;
    capabilities.supported_intents =
        image::raw_development_intent_mask(image::RawDevelopmentIntent::preview)
        | image::raw_development_intent_mask(image::RawDevelopmentIntent::detail)
        | image::raw_development_intent_mask(image::RawDevelopmentIntent::export_image);
    capabilities.supported_qualities =
        image::raw_development_quality_mask(image::RawDevelopmentQuality::balanced);
    capabilities.supported_dng_opcode_policies =
        image::dng_opcode_policy_mask(image::DngOpcodePolicy::provider_default);
    capabilities.supported_noise_reduction_intents =
        image::raw_noise_reduction_intent_mask(image::RawNoiseReductionIntent::provider_default);
    capabilities.supported_highlight_recovery_intents =
        image::raw_highlight_recovery_intent_mask(
            image::RawHighlightRecoveryIntent::provider_default
        );
    return capabilities;
}

[[nodiscard]] image::RawFrame synthetic_bayer_frame(const bool with_matrix = true) {
    image::RawFrame frame;
    auto& descriptor = frame.descriptor;
    descriptor.provider_id = "synthetic-provider";
    descriptor.provider_version = "synthetic-v1";
    descriptor.storage_dimensions = {4U, 4U};
    descriptor.active_dimensions = descriptor.storage_dimensions;
    descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    descriptor.cfa_pattern = "RGGB";
    descriptor.bits_per_sample = 10U;
    descriptor.black_levels = {0U, 0U, 0U, 0U};
    descriptor.white_levels = {1'000U, 1'000U, 1'000U, 1'000U};
    descriptor.as_shot_neutral = {0.5, 1.0, 1.0, 0.25};
    if (with_matrix) {
        descriptor.camera_to_linear_srgb_d65 = {
            1.0, 0.0, 0.0,
            0.0, 1.0, 0.0,
            0.0, 0.0, 1.0,
        };
        descriptor.has_camera_to_linear_srgb_d65 = true;
    }
    frame.samples.resize(16U);
    for (std::uint32_t y = 0U; y < 4U; ++y) {
        for (std::uint32_t x = 0U; x < 4U; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = descriptor.bayer_2x2[site];
            const std::uint16_t value = colour == image::RawCfaColor::red
                ? 100U : colour == image::RawCfaColor::green ? 200U : 50U;
            frame.samples[static_cast<std::size_t>(y) * 4U + x] = value;
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame noisy_bayer_frame() {
    auto frame = synthetic_bayer_frame();
    auto& descriptor = frame.descriptor;
    descriptor.storage_dimensions = {12U, 12U};
    descriptor.active_dimensions = descriptor.storage_dimensions;
    descriptor.white_levels = {4'095U, 4'095U, 4'095U, 4'095U};
    descriptor.sensor_noise = {
        .schema_version = image::raw_sensor_noise_calibration_schema_version,
        .model = image::RawSensorNoiseModel::poisson_gaussian_per_cfa,
        .source = image::RawSensorNoiseCalibrationSource::provider_calibration_profile,
        .iso_sensitivity = 6'400.0,
        .read_noise_stddev_dn = {42.0, 39.0, 39.0, 44.0},
        .shot_noise_variance_per_dn = {0.08, 0.08, 0.08, 0.09},
    };
    frame.samples.resize(12U * 12U);
    constexpr std::array<std::uint16_t, 4U> flat_signal{600U, 1'200U, 1'160U, 400U};
    constexpr std::array<int, 4U> perturbation{-72, 56, -44, 68};
    for (std::uint32_t y = 0U; y < descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const int phase = static_cast<int>(((x / 2U) + (y / 2U)) & 1U);
            const int sample = static_cast<int>(flat_signal[site])
                + (phase == 0 ? perturbation[site] : -perturbation[site]);
            frame.samples[static_cast<std::size_t>(y) * descriptor.storage_dimensions.width + x]
                = static_cast<std::uint16_t>(sample);
        }
    }
    return frame;
}

[[nodiscard]] std::uint64_t flat_cfa_error(const image::RawFrame& frame) {
    constexpr std::array<std::uint16_t, 4U> flat_signal{600U, 1'200U, 1'160U, 400U};
    const auto width = frame.descriptor.storage_dimensions.width;
    std::uint64_t total = 0U;
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto sample = frame.samples[static_cast<std::size_t>(y) * width + x];
            total += static_cast<std::uint64_t>(std::abs(
                static_cast<int>(sample) - static_cast<int>(flat_signal[site])
            ));
        }
    }
    return total;
}

[[nodiscard]] image::PixelBuffer processed_fallback() {
    image::PixelBuffer buffer;
    buffer.dimensions = {4U, 4U};
    buffer.bits_per_channel = 16U;
    buffer.channels = 3U;
    buffer.row_stride_bytes = 4U * 3U * sizeof(std::uint16_t);
    buffer.primaries = image::RgbPrimaries::srgb_rec709_d65;
    buffer.transfer_function = image::RgbTransferFunction::linear;
    buffer.reference = image::RgbBufferReference::processed_raw;
    buffer.samples.assign(4U * 4U * 3U, 7'777U);
    return buffer;
}

class SyntheticRawSession final : public image::DecodeSession {
public:
    explicit SyntheticRawSession(
        image::RawFrame frame,
        std::string normalized_make = {},
        std::string normalized_model = {}
    ) : frame_(std::move(frame)) {
        metadata_.raw_dimensions = {4U, 4U};
        metadata_.image_dimensions = {4U, 4U};
        metadata_.normalized_make = std::move(normalized_make);
        metadata_.normalized_model = std::move(normalized_model);
        capabilities_.metadata = true;
        capabilities_.raw_frame = true;
        capabilities_.reference_rgb = true;
        capabilities_.raw_development = raw_capabilities();
    }

    [[nodiscard]] const image::AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const image::DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const image::PreviewDescriptor> previews() const noexcept override {
        return {};
    }

    [[nodiscard]] image::PreviewPayload decode_preview(std::size_t) override {
        throw image::DecodeError(image::DecodeErrorCode::no_preview, 0, "no preview");
    }

    [[nodiscard]] image::RawFrame decode_raw_frame() override {
        ++raw_frame_count_;
        return frame_;
    }

    [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
        ++processed_count_;
        return processed_fallback();
    }

    [[nodiscard]] std::size_t raw_frame_count() const noexcept {
        return raw_frame_count_;
    }

    [[nodiscard]] std::size_t processed_count() const noexcept {
        return processed_count_;
    }

private:
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
    image::RawFrame frame_;
    std::size_t raw_frame_count_ = 0U;
    mutable std::size_t processed_count_ = 0U;
};

[[nodiscard]] image::DcpMatrix3x3 diagonal_matrix(
    const double first,
    const double second,
    const double third
) {
    return image::DcpMatrix3x3{{
        first, 0.0, 0.0,
        0.0, second, 0.0,
        0.0, 0.0, third,
    }};
}

[[nodiscard]] image::CameraProfileCatalog exact_dcp_catalog() {
    image::DcpProfile profile;
    profile.unique_camera_model = "OPEN CAMERA MK I";
    profile.profile_name = "Open Camera DCP";
    profile.calibration1.illuminant = 21U;
    profile.calibration1.illuminant_was_explicit = true;
    profile.calibration1.color_matrix = diagonal_matrix(
        1.0 / 0.95047,
        1.0,
        1.0 / 1.08883
    );
    profile.calibration1.forward_matrix = diagonal_matrix(
        0.964295676,
        1.0,
        0.825104603
    );
    profile.baseline_exposure_offset_ev = 1.0;
    return image::CameraProfileCatalog{
        .profiles = {
            image::CameraProfileDefinition{
                .profile = std::move(profile),
                .normalized_camera_model = "OPEN CAMERA MK I",
                .content_identity = "sha256:synthetic-open-camera-dcp",
                .source_name = "open-camera.dcp",
            },
        },
        .identity = "shadow-camera-profile-catalog-v1:test",
    };
}

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
        developed.pixels.dimensions == image::Dimensions{2U, 2U}
            && developed.pixels.reference == image::RgbBufferReference::processed_raw,
        "area preview produces bounded standardized scene-linear RGB"
    );
    expect(
        session.raw_frame_count() == 1U && session.processed_count() == 0U,
        "supported RawFrame preparation never asks the provider to develop RGB"
    );
    expect(
        developed.pixels.raw_development_receipt.development_settings_signature.find(
            "bayer-area-preview"
        ) != std::string::npos,
        "preview receipt distinguishes CFA area integration from full bilinear development"
    );
    for (std::size_t pixel = 0U; pixel < 4U; ++pixel) {
        const std::size_t index = pixel * 3U;
        const auto red = developed.pixels.samples[index];
        const auto green = developed.pixels.samples[index + 1U];
        const auto blue = developed.pixels.samples[index + 2U];
        expect(
            red == green && green == blue && red >= 13'106U && red <= 13'108U,
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
        developed.pixels.raw_development_receipt.development_settings_signature.find(
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
            && developed.pixels.samples.front() == 7'777U,
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
            && developed.pipeline_receipt.camera_profile_developer_version == 1U,
        "DCP content and developer identities are cache-visible"
    );
    expect(
        session.raw_frame_count() == 1U && session.processed_count() == 0U,
        "DCP-calibrated RawFrame never asks provider for processed RGB"
    );
    for (std::size_t pixel = 0U; pixel < 4U; ++pixel) {
        const std::size_t index = pixel * 3U;
        expect(
            developed.pixels.samples[index] >= 26'210U
                && developed.pixels.samples[index] <= 26'220U
                && developed.pixels.samples[index + 1U]
                    == developed.pixels.samples[index]
                && developed.pixels.samples[index + 2U]
                    == developed.pixels.samples[index],
            "DCP ForwardMatrix keeps the neutral and applies BaselineExposureOffset"
        );
    }
}

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
}

} // namespace

int main() {
    automatic_pipeline_prefers_owned_raw_frame();
    full_pipeline_records_the_effective_backend_in_every_identity();
    unsupported_host_stage_falls_back_explicitly();
    exact_dcp_replaces_missing_generic_matrix();
    raw_denoise_is_cfa_preserving_and_preview_aware();
    return failures == 0 ? 0 : 1;
}
