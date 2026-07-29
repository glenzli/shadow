#pragma once

#include <shadow/image/raw_denoise.hpp>

namespace shadow::image::detail {

// Resolves product intent, source calibration, and cache-visible semantics before choosing an
// executor. The prepared plan can be consumed by the standalone CPU/Metal stage or by the
// resident Metal RAW transaction without duplicating policy in either backend.
struct PreparedRawBayerDenoise final {
    RawBayerDenoiseMode mode = RawBayerDenoiseMode::skipped;
    double iso_sensitivity = 0.0;
    RawBayerDenoiseReceipt receipt;

    [[nodiscard]] bool applied() const noexcept {
        return mode != RawBayerDenoiseMode::skipped;
    }
};

[[nodiscard]] PreparedRawBayerDenoise
prepare_raw_bayer_denoise(const RawFrame& frame, const RawBayerDenoiseRequest& request);

[[nodiscard]] RawBayerDenoiseReceipt finalize_raw_bayer_denoise_receipt(
    const RawFrame& frame,
    const PreparedRawBayerDenoise& prepared,
    RawBayerDenoiseBackend backend
);

// Executes an already-resolved plan with the configured standalone backend. This remains the
// fallback for callers that need a materialized RawFrame and for resident transactions that
// exceed a device working-set limit.
[[nodiscard]] RawBayerDenoiseResult
execute_prepared_raw_bayer_denoise(RawFrame frame, const PreparedRawBayerDenoise& prepared);

} // namespace shadow::image::detail
