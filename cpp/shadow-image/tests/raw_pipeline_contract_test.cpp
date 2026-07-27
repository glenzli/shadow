#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/edit.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_denoise.hpp>
#include <shadow/image/source_rendering.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
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
        metadata_.raw_dimensions = frame_.descriptor.active_dimensions;
        metadata_.image_dimensions = frame_.descriptor.active_dimensions;
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

[[nodiscard]] image::RawFrame gradient_bayer_frame() {
    auto frame = synthetic_bayer_frame();
    auto& descriptor = frame.descriptor;
    descriptor.storage_dimensions = {32U, 32U};
    descriptor.active_dimensions = descriptor.storage_dimensions;
    descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
    descriptor.white_levels = {1'000U, 1'000U, 1'000U, 1'000U};
    frame.samples.resize(32U * 32U);
    for (std::uint32_t y = 0U; y < 32U; ++y) {
        for (std::uint32_t x = 0U; x < 32U; ++x) {
            const double normalized = 0.08 + 0.56
                * static_cast<double>(x + y) / 62.0;
            frame.samples[static_cast<std::size_t>(y) * 32U + x] =
                static_cast<std::uint16_t>(std::round(normalized * 1'000.0));
        }
    }
    return frame;
}

[[nodiscard]] image::DcpHsvTable identity_hue_sat_table() {
    return image::DcpHsvTable{
        .hue_divisions = 1U,
        .saturation_divisions = 2U,
        .value_divisions = 1U,
        .encoding = image::DcpTableEncoding::linear,
        .entries = {
            image::DcpHsvDelta{
                .hue_shift_degrees = 0.0F,
                .saturation_scale = 1.0F,
                .value_scale = 1.0F,
            },
            image::DcpHsvDelta{
                .hue_shift_degrees = 0.0F,
                .saturation_scale = 1.0F,
                .value_scale = 1.0F,
            },
        },
    };
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
    // Keep the DCP post-stage route active while intentionally producing
    // scene-linear values above display white. The pipeline must retain this
    // fp32 headroom rather than falling back to a packed u16 buffer merely to
    // execute a camera profile's HueSatMap.
    profile.calibration1.hue_sat_map = identity_hue_sat_table();
    profile.baseline_exposure_offset_ev = 3.0;
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
            && developed.pipeline_receipt.camera_profile_developer_version == 1U,
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
    const auto rejected = image::negotiate_shadow_raw_frame_development_plan(host_plan);
    expect(
        !rejected.accepted(),
        "unsupported host sensor stages are rejected explicitly instead of being silently downgraded"
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
    automatic_pipeline_prefers_owned_raw_frame();
    full_pipeline_records_the_effective_backend_in_every_identity();
    unsupported_host_stage_falls_back_explicitly();
    exact_dcp_replaces_missing_generic_matrix();
    raw_denoise_is_cfa_preserving_and_preview_aware();
    raw_denoise_execution_and_calibration_are_cache_visible();
    raw_highlight_treatment_is_executed_and_cache_visible();
    high_quality_raw_plan_is_executed_and_cache_visible();
    host_raw_frame_capabilities_are_not_limited_by_provider_rgb_fallbacks();
    raw_frame_source_calibration_is_identical_for_preview_and_detail();
    return failures == 0 ? 0 : 1;
}
