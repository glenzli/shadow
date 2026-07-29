#include "raw_denoise_plan.hpp"

#include <shadow/image/decoder_error.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace shadow::image {

namespace {

constexpr double automatic_minimum_iso = 800.0;

[[nodiscard]] bool finite_positive(const double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] double
resolved_iso(const RawFrame& frame, const RawBayerDenoiseRequest& request) noexcept {
    if (finite_positive(request.iso_sensitivity)) {
        return request.iso_sensitivity;
    }
    const auto& calibration = frame.descriptor.sensor_noise;
    return calibration.valid() && finite_positive(calibration.iso_sensitivity)
               ? calibration.iso_sensitivity
               : 0.0;
}

[[nodiscard]] RawBayerDenoiseMode
resolve_mode(const RawFrame& frame, const RawBayerDenoiseRequest& request) noexcept {
    switch (request.intent) {
    case RawNoiseReductionIntent::disabled:
        return RawBayerDenoiseMode::skipped;
    case RawNoiseReductionIntent::conservative:
        return RawBayerDenoiseMode::cfa_bilateral_conservative_v1;
    case RawNoiseReductionIntent::noise_robust:
        return RawBayerDenoiseMode::cfa_bilateral_noise_robust_v1;
    case RawNoiseReductionIntent::provider_default:
        if (request.preview || resolved_iso(frame, request) < automatic_minimum_iso) {
            return RawBayerDenoiseMode::skipped;
        }
        return RawBayerDenoiseMode::cfa_bilateral_conservative_v1;
    }
    return RawBayerDenoiseMode::skipped;
}

[[nodiscard]] RawNoiseReductionIntent effective_intent(const RawBayerDenoiseMode mode) noexcept {
    switch (mode) {
    case RawBayerDenoiseMode::skipped:
        return RawNoiseReductionIntent::disabled;
    case RawBayerDenoiseMode::cfa_bilateral_conservative_v1:
        return RawNoiseReductionIntent::conservative;
    case RawBayerDenoiseMode::cfa_bilateral_noise_robust_v1:
        return RawNoiseReductionIntent::noise_robust;
    }
    return RawNoiseReductionIntent::disabled;
}

[[nodiscard]] const char* intent_identity(const RawNoiseReductionIntent intent) noexcept {
    switch (intent) {
    case RawNoiseReductionIntent::provider_default:
        return "provider-default";
    case RawNoiseReductionIntent::disabled:
        return "disabled";
    case RawNoiseReductionIntent::conservative:
        return "conservative";
    case RawNoiseReductionIntent::noise_robust:
        return "noise-robust";
    }
    return "unknown";
}

[[nodiscard]] const char*
calibration_source_identity(const RawSensorNoiseCalibrationSource source) noexcept {
    switch (source) {
    case RawSensorNoiseCalibrationSource::unavailable:
        return "unavailable";
    case RawSensorNoiseCalibrationSource::embedded_metadata:
        return "embedded-metadata";
    case RawSensorNoiseCalibrationSource::provider_calibration_profile:
        return "provider-profile";
    }
    return "unknown";
}

void append_double_bits(std::ostringstream& output, const double value) {
    output << std::hex << std::setw(16) << std::setfill('0') << std::bit_cast<std::uint64_t>(value)
           << std::dec;
}

[[nodiscard]] std::string denoise_cache_identity(
    const RawFrame& frame,
    const RawBayerDenoiseReceipt& receipt,
    const double iso
) {
    std::ostringstream identity;
    identity << "raw-denoise=" << raw_bayer_denoise_mode_identity(receipt.mode)
             << ";raw-denoise-request=" << intent_identity(receipt.requested_intent)
             << ";raw-denoise-effective=" << intent_identity(receipt.effective_intent)
             << ";raw-denoise-backend=" << raw_bayer_denoise_backend_identity(receipt.backend);
    if (!receipt.applied()) {
        identity << ";raw-denoise-model=none";
        return identity.str();
    }

    const auto& calibration = frame.descriptor.sensor_noise;
    if (receipt.used_sensor_noise_calibration) {
        identity << ";raw-denoise-model=poisson-gaussian-per-cfa-v1"
                 << ";raw-denoise-calibration-source="
                 << calibration_source_identity(calibration.source)
                 << ";raw-denoise-calibration-iso=";
        append_double_bits(identity, calibration.iso_sensitivity);
        identity << ";raw-denoise-read=";
        for (const double value : calibration.read_noise_stddev_dn) {
            append_double_bits(identity, value);
        }
        identity << ";raw-denoise-shot=";
        for (const double value : calibration.shot_noise_variance_per_dn) {
            append_double_bits(identity, value);
        }
        return identity.str();
    }

    identity << ";raw-denoise-model=iso-fallback-v1;raw-denoise-iso=";
    append_double_bits(identity, iso);
    return identity.str();
}

} // namespace

const char* raw_bayer_denoise_mode_identity(const RawBayerDenoiseMode mode) noexcept {
    switch (mode) {
    case RawBayerDenoiseMode::skipped:
        return "skipped";
    case RawBayerDenoiseMode::cfa_bilateral_conservative_v1:
        return "cfa-bilateral-conservative-v1";
    case RawBayerDenoiseMode::cfa_bilateral_noise_robust_v1:
        return "cfa-bilateral-noise-robust-v1";
    }
    return "unknown";
}

const char* raw_bayer_denoise_backend_identity(const RawBayerDenoiseBackend backend) noexcept {
    switch (backend) {
    case RawBayerDenoiseBackend::cpu:
        return "cpu";
    case RawBayerDenoiseBackend::metal:
        return "metal";
    }
    return "unknown";
}

bool RawBayerDenoiseReceipt::valid() const noexcept {
    if (schema_version != raw_bayer_denoise_receipt_schema_version) {
        return false;
    }
    if (backend != RawBayerDenoiseBackend::cpu && backend != RawBayerDenoiseBackend::metal) {
        return false;
    }
    if (cache_identity.rfind("raw-denoise=", 0U) != 0U) {
        return false;
    }
    switch (mode) {
    case RawBayerDenoiseMode::skipped:
        return effective_intent == RawNoiseReductionIntent::disabled
               && !used_sensor_noise_calibration && backend == RawBayerDenoiseBackend::cpu
               && (requested_intent == RawNoiseReductionIntent::disabled
                   || requested_intent == RawNoiseReductionIntent::provider_default);
    case RawBayerDenoiseMode::cfa_bilateral_conservative_v1:
        return effective_intent == RawNoiseReductionIntent::conservative
               && (requested_intent == RawNoiseReductionIntent::conservative
                   || requested_intent == RawNoiseReductionIntent::provider_default);
    case RawBayerDenoiseMode::cfa_bilateral_noise_robust_v1:
        return effective_intent == RawNoiseReductionIntent::noise_robust
               && requested_intent == RawNoiseReductionIntent::noise_robust;
    }
    return false;
}

namespace detail {

PreparedRawBayerDenoise
prepare_raw_bayer_denoise(const RawFrame& frame, const RawBayerDenoiseRequest& request) {
    if (!frame.valid() || !frame.is_bayer_2x2()) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "RAW denoise requires a valid Bayer two-by-two RawFrame"
        );
    }

    const double iso = resolved_iso(frame, request);
    const RawBayerDenoiseMode mode = resolve_mode(frame, request);
    return PreparedRawBayerDenoise{
        .mode = mode,
        .iso_sensitivity = iso,
        .receipt = RawBayerDenoiseReceipt{
            .schema_version = raw_bayer_denoise_receipt_schema_version,
            .requested_intent = request.intent,
            .effective_intent = effective_intent(mode),
            .mode = mode,
            .used_sensor_noise_calibration =
                mode != RawBayerDenoiseMode::skipped && frame.descriptor.sensor_noise.valid()
                && frame.descriptor.sensor_noise.model
                       == RawSensorNoiseModel::poisson_gaussian_per_cfa,
        },
    };
}

RawBayerDenoiseReceipt finalize_raw_bayer_denoise_receipt(
    const RawFrame& frame,
    const PreparedRawBayerDenoise& prepared,
    const RawBayerDenoiseBackend backend
) {
    RawBayerDenoiseReceipt receipt = prepared.receipt;
    receipt.backend = backend;
    receipt.cache_identity = denoise_cache_identity(frame, receipt, prepared.iso_sensitivity);
    return receipt;
}

} // namespace detail
} // namespace shadow::image
