#include <shadow/image/raw_denoise.hpp>

#include <shadow/image/decoder_error.hpp>

#include "metal_raw_development.hpp"
#include "../concurrency/row_scheduler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

constexpr double automatic_minimum_iso = 800.0;

[[nodiscard]] std::size_t cfa_site(const std::uint32_t x, const std::uint32_t y) noexcept {
    return static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
}

[[nodiscard]] bool finite_positive(const double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] double resolved_iso(
    const RawFrame& frame,
    const RawBayerDenoiseRequest& request
) noexcept {
    if (finite_positive(request.iso_sensitivity)) {
        return request.iso_sensitivity;
    }
    const auto& calibration = frame.descriptor.sensor_noise;
    return calibration.valid() && finite_positive(calibration.iso_sensitivity)
        ? calibration.iso_sensitivity : 0.0;
}

[[nodiscard]] RawBayerDenoiseMode resolve_mode(
    const RawFrame& frame,
    const RawBayerDenoiseRequest& request
) noexcept {
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

[[nodiscard]] RawNoiseReductionIntent effective_intent(
    const RawBayerDenoiseMode mode
) noexcept {
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

[[nodiscard]] const char* intent_identity(
    const RawNoiseReductionIntent intent
) noexcept {
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

[[nodiscard]] const char* calibration_source_identity(
    const RawSensorNoiseCalibrationSource source
) noexcept {
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
    output << std::hex << std::setw(16) << std::setfill('0')
           << std::bit_cast<std::uint64_t>(value) << std::dec;
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
             << ";raw-denoise-backend="
             << raw_bayer_denoise_backend_identity(receipt.backend);
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

[[nodiscard]] RawBayerDenoiseReceipt finalize_receipt(
    const RawFrame& frame,
    RawBayerDenoiseReceipt receipt,
    const double iso
) {
    receipt.cache_identity = denoise_cache_identity(frame, receipt, iso);
    return receipt;
}

[[nodiscard]] double fallback_noise_stddev_dn(
    const RawFrameDescriptor& descriptor,
    const std::size_t site,
    const double iso
) noexcept {
    const double range = static_cast<double>(descriptor.white_levels[site])
        - static_cast<double>(descriptor.black_levels[site]);
    // This is deliberately a modest, documented fallback for public LibRaw sources that do not
    // yet expose calibrated noise coefficients. It scales with sensor range and ISO but does
    // not pretend to be a camera-specific profile. A provider calibration always wins below.
    const double iso_multiplier = finite_positive(iso) ? std::sqrt(iso / 100.0) : 1.0;
    return std::max(1.0, range * (0.0015 + 0.00035 * iso_multiplier));
}

[[nodiscard]] double noise_stddev_dn(
    const RawFrame& frame,
    const std::size_t site,
    const std::uint16_t sample,
    const double iso
) noexcept {
    const auto& descriptor = frame.descriptor;
    const auto& calibration = descriptor.sensor_noise;
    if (
        calibration.valid()
        && calibration.model == RawSensorNoiseModel::poisson_gaussian_per_cfa
    ) {
        const double signal = std::max(
            0.0,
            static_cast<double>(sample) - static_cast<double>(descriptor.black_levels[site])
        );
        return std::sqrt(
            calibration.read_noise_stddev_dn[site] * calibration.read_noise_stddev_dn[site]
            + calibration.shot_noise_variance_per_dn[site] * signal
        );
    }
    return fallback_noise_stddev_dn(descriptor, site, iso);
}

[[nodiscard]] std::uint16_t filtered_sample(
    const RawFrame& frame,
    const std::vector<std::uint16_t>& source,
    const std::uint32_t x,
    const std::uint32_t y,
    const RawBayerDenoiseMode mode,
    const double iso
) noexcept {
    const auto& descriptor = frame.descriptor;
    const std::uint32_t width = descriptor.storage_dimensions.width;
    const std::uint32_t left = descriptor.active_margins.left;
    const std::uint32_t top = descriptor.active_margins.top;
    const std::uint32_t right = left + descriptor.active_dimensions.width;
    const std::uint32_t bottom = top + descriptor.active_dimensions.height;
    const std::size_t site = cfa_site(x, y);
    const auto source_index = static_cast<std::size_t>(y) * width + x;
    const std::uint16_t center = source[source_index];
    const double sigma = noise_stddev_dn(frame, site, center, iso);
    const int radius = mode == RawBayerDenoiseMode::cfa_bilateral_noise_robust_v1 ? 2 : 1;
    const double range_scale = mode == RawBayerDenoiseMode::cfa_bilateral_noise_robust_v1
        ? 3.0 : 2.25;
    const double blend = mode == RawBayerDenoiseMode::cfa_bilateral_noise_robust_v1
        ? 0.90 : 0.62;
    const double range = std::max(1.0, sigma * range_scale);
    const double inverse_range_squared = 1.0 / (range * range);

    double weighted_total = 0.0;
    double total_weight = 0.0;
    for (int offset_y = -radius; offset_y <= radius; ++offset_y) {
        const auto candidate_y = static_cast<std::int64_t>(y) + offset_y * 2;
        if (candidate_y < static_cast<std::int64_t>(top)
            || candidate_y >= static_cast<std::int64_t>(bottom)) {
            continue;
        }
        for (int offset_x = -radius; offset_x <= radius; ++offset_x) {
            // Automatic conservative mode is deliberately a same-colour cross instead of a
            // 3x3 square. It keeps the first RAW stage inexpensive enough for interactive
            // detail preparation; the explicit robust mode owns the wider 5x5 neighbourhood.
            if (
                mode == RawBayerDenoiseMode::cfa_bilateral_conservative_v1
                && std::abs(offset_x) + std::abs(offset_y) > 1
            ) {
                continue;
            }
            const auto candidate_x = static_cast<std::int64_t>(x) + offset_x * 2;
            if (candidate_x < static_cast<std::int64_t>(left)
                || candidate_x >= static_cast<std::int64_t>(right)) {
                continue;
            }
            const auto candidate_index = static_cast<std::size_t>(candidate_y) * width
                + static_cast<std::uint32_t>(candidate_x);
            const std::uint16_t candidate = source[candidate_index];
            const double delta = static_cast<double>(candidate) - static_cast<double>(center);
            const double spatial = 1.0 / static_cast<double>(
                1 + offset_x * offset_x + offset_y * offset_y
            );
            // A bounded rational approximation avoids one expensive transcendental call for
            // every raw-neighbour pair. It is still monotone, edge preserving, and maps an
            // equal-valued neighbour to one; unlike an exponential it keeps high-ISO detail
            // preparation within an interactive desktop budget on 45–50 MP frames.
            const double range_weight = 1.0 / (1.0 + delta * delta * inverse_range_squared);
            const double weight = spatial * range_weight;
            weighted_total += static_cast<double>(candidate) * weight;
            total_weight += weight;
        }
    }
    if (total_weight <= 0.0 || !std::isfinite(total_weight)) {
        return center;
    }
    const double filtered = weighted_total / total_weight;
    const double blended = static_cast<double>(center) + (filtered - center) * blend;
    return static_cast<std::uint16_t>(std::clamp(
        std::lround(blended),
        0L,
        static_cast<long>(std::numeric_limits<std::uint16_t>::max())
    ));
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
            && !used_sensor_noise_calibration
            && backend == RawBayerDenoiseBackend::cpu
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

RawBayerDenoiseResult denoise_bayer_raw_frame(
    RawFrame frame,
    const RawBayerDenoiseRequest& request
) {
    if (!frame.valid() || !frame.is_bayer_2x2()) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "RAW denoise requires a valid Bayer two-by-two RawFrame"
        );
    }

    const double iso = resolved_iso(frame, request);
    const RawBayerDenoiseMode mode = resolve_mode(frame, request);
    RawBayerDenoiseReceipt receipt{
        .schema_version = raw_bayer_denoise_receipt_schema_version,
        .requested_intent = request.intent,
        .effective_intent = effective_intent(mode),
        .mode = mode,
        .used_sensor_noise_calibration = mode != RawBayerDenoiseMode::skipped
            && frame.descriptor.sensor_noise.model
                == RawSensorNoiseModel::poisson_gaussian_per_cfa,
    };
    if (mode == RawBayerDenoiseMode::skipped) {
        auto finalized_receipt = finalize_receipt(frame, std::move(receipt), iso);
        return RawBayerDenoiseResult{
            .frame = std::move(frame),
            .receipt = std::move(finalized_receipt),
        };
    }

    // Raw denoise is a large, regular same-CFA stencil and is therefore a good Metal candidate.
    // The execution choice shares the existing developer setting: automatic tries Metal then
    // falls back transparently, CPU disables it, and a forced Metal request fails closed rather
    // than silently claiming an acceleration that did not happen.
    const RawDevelopmentBackendMode backend_mode = raw_development_backend_mode_from_environment();
    if (backend_mode != RawDevelopmentBackendMode::cpu) {
        auto metal_attempt = detail::try_denoise_bayer_raw_frame_metal(
            frame,
            mode,
            iso
        );
        if (metal_attempt.applied) {
            receipt.backend = RawBayerDenoiseBackend::metal;
            auto finalized_receipt = finalize_receipt(frame, std::move(receipt), iso);
            return RawBayerDenoiseResult{
                .frame = std::move(frame),
                .receipt = std::move(finalized_receipt),
            };
        }
        if (backend_mode == RawDevelopmentBackendMode::metal) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                metal_attempt.diagnostic.empty()
                    ? "Metal RAW denoise is unavailable" : std::move(metal_attempt.diagnostic)
            );
        }
    }

    const std::vector<std::uint16_t> source = frame.samples;
    const auto& descriptor = frame.descriptor;
    const std::uint32_t left = descriptor.active_margins.left;
    const std::uint32_t top = descriptor.active_margins.top;
    const std::uint32_t right = left + descriptor.active_dimensions.width;
    const std::uint32_t bottom = top + descriptor.active_dimensions.height;
    const std::uint32_t width = descriptor.storage_dimensions.width;
    detail::parallel_for_rows(
        bottom - top,
        16U,
        [&frame, &source, left, top, right, width, mode, iso](
            const std::uint32_t first_row,
            const std::uint32_t last_row
        ) {
            for (std::uint32_t relative_y = first_row; relative_y < last_row; ++relative_y) {
                detail::throw_if_row_cancelled();
                const std::uint32_t y = top + relative_y;
                auto* destination = frame.samples.data()
                    + static_cast<std::size_t>(y) * width + left;
                for (std::uint32_t x = left; x < right; ++x) {
                    *destination++ = filtered_sample(frame, source, x, y, mode, iso);
                }
            }
        }
    );
    auto finalized_receipt = finalize_receipt(frame, std::move(receipt), iso);
    return RawBayerDenoiseResult{
        .frame = std::move(frame),
        .receipt = std::move(finalized_receipt),
    };
}

} // namespace shadow::image
