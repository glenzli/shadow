#pragma once

#include <shadow/image/decoder.hpp>

#include <cstdint>

namespace shadow::image {

// The first owned RAW denoise stage works on the untouched Bayer plane before demosaic. It
// never averages distinct CFA sites, so it cannot invent a colour value by blending red, green,
// and blue measurements together. Its result stays a RawFrame: black level, white balance,
// camera colour, highlight treatment, and all later RGB edits remain separate stages.
inline constexpr std::uint32_t raw_bayer_denoise_receipt_schema_version = 1U;

enum class RawBayerDenoiseMode : std::uint8_t {
    skipped,
    cfa_bilateral_conservative_v1,
    cfa_bilateral_noise_robust_v1,
};

struct RawBayerDenoiseRequest final {
    RawNoiseReductionIntent intent = RawNoiseReductionIntent::provider_default;
    // The source metadata supplies ISO independently from RawFrame. Zero means unknown, not
    // ISO 100. Automatic mode then declines a profile-free reduction rather than guessing.
    double iso_sensitivity = 0.0;
    // An ordinary bounded preview already gets substantial noise averaging from CFA-aware area
    // sampling. Keep it responsive in automatic mode; an explicit user request may still run.
    bool preview = false;
};

struct RawBayerDenoiseReceipt final {
    std::uint32_t schema_version = raw_bayer_denoise_receipt_schema_version;
    RawNoiseReductionIntent requested_intent = RawNoiseReductionIntent::provider_default;
    RawNoiseReductionIntent effective_intent = RawNoiseReductionIntent::disabled;
    RawBayerDenoiseMode mode = RawBayerDenoiseMode::skipped;
    bool used_sensor_noise_calibration = false;

    [[nodiscard]] bool applied() const noexcept {
        return mode != RawBayerDenoiseMode::skipped;
    }

    [[nodiscard]] bool valid() const noexcept;
};

struct RawBayerDenoiseResult final {
    RawFrame frame;
    RawBayerDenoiseReceipt receipt;
};

[[nodiscard]] const char* raw_bayer_denoise_mode_identity(
    RawBayerDenoiseMode mode
) noexcept;

// Uses a per-CFA bilateral estimator only when the request makes that truthful: automatic mode
// is intentionally limited to high-ISO detail/export work, while explicit conservative/robust
// requests are honored for both preview and full-resolution development. A validated numeric
// sensor model is preferred; otherwise the stage uses its documented ISO-scaled fallback rather
// than reading opaque provider calibration data.
[[nodiscard]] RawBayerDenoiseResult denoise_bayer_raw_frame(
    RawFrame frame,
    const RawBayerDenoiseRequest& request
);

} // namespace shadow::image
