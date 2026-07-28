#pragma once

#include "raw_pipeline_contract_test_support.hpp"

namespace {

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

} // namespace
