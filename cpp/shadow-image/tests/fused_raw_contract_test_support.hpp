#pragma once

#include <shadow/image/fused_raw_development.hpp>

#include <cstddef>
#include <cstdint>

namespace image = shadow::image;

namespace {

[[nodiscard]] image::RawFrame synthetic_frame(const std::int32_t orientation) {
    image::RawFrame frame;
    auto& descriptor = frame.descriptor;
    descriptor.provider_id = "fused-contract";
    descriptor.provider_version = "v1";
    descriptor.storage_dimensions = {8U, 6U};
    descriptor.active_dimensions = {6U, 4U};
    descriptor.active_margins = {
        .left = 1U,
        .top = 1U,
        .right = 1U,
        .bottom = 1U,
    };
    descriptor.orientation = orientation;
    descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    descriptor.cfa_pattern = "RGGB";
    descriptor.bits_per_sample = 12U;
    descriptor.black_levels = {10U, 20U, 30U, 40U};
    descriptor.white_levels = {1'010U, 1'020U, 1'030U, 1'040U};
    descriptor.as_shot_neutral = {0.5, 1.0, 1.0, 0.25};
    frame.samples.resize(8U * 6U);
    for (std::uint32_t y = 0U; y < 6U; ++y) {
        for (std::uint32_t x = 0U; x < 8U; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const std::uint32_t signal =
                80U + x * 17U + y * 29U + static_cast<std::uint32_t>(site) * 23U;
            frame.samples[static_cast<std::size_t>(y) * 8U + x] =
                static_cast<std::uint16_t>(descriptor.black_levels[site] + signal);
        }
    }
    return frame;
}

} // namespace
