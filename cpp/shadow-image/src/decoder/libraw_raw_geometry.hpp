#pragma once

#include <shadow/image/decoder_types.hpp>

#include <cstdint>

namespace shadow::image::detail {

// `raw_inset_crops[0]` is LibRaw's standard active RAW crop.  Some cameras expose a
// stored Bayer rectangle that is wider than that crop; preserve the stored samples while
// making RawFrame's active area agree with the camera-defined RAW image rectangle.  The
// second crop is vendor-specific and deliberately remains out of this provider-neutral
// contract.
inline constexpr std::uint32_t libraw_raw_frame_geometry_contract_version = 2U;

struct LibRawRawFrameGeometry final {
    Dimensions active_dimensions{};
    Margins active_margins{};
};

struct LibRawRawInsetCrop final {
    std::uint32_t left = 0U;
    std::uint32_t top = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

// Keep this small projection independent of libraw.h: geometry policy is a Shadow contract,
// while the decoder alone owns the ABI-specific extraction of the LibRaw fields.
struct LibRawRawFrameGeometryInput final {
    std::uint32_t raw_width = 0U;
    std::uint32_t raw_height = 0U;
    std::uint32_t legacy_width = 0U;
    std::uint32_t legacy_height = 0U;
    std::uint32_t legacy_left = 0U;
    std::uint32_t legacy_top = 0U;
    LibRawRawInsetCrop standard_inset{};
};

[[nodiscard]] inline bool
valid_standard_raw_inset(const LibRawRawFrameGeometryInput& input) noexcept {
    const auto& inset = input.standard_inset;
    const std::uint32_t right = inset.left + inset.width;
    const std::uint32_t bottom = inset.top + inset.height;
    return inset.width != 0U && inset.height != 0U && right <= input.raw_width
           && bottom <= input.raw_height;
}

[[nodiscard]] inline LibRawRawFrameGeometry
raw_frame_geometry(const LibRawRawFrameGeometryInput& input) noexcept {
    if (valid_standard_raw_inset(input)) {
        const auto& inset = input.standard_inset;
        return {
            Dimensions{inset.width, inset.height},
            Margins{
                inset.left,
                inset.top,
                input.raw_width - inset.left - inset.width,
                input.raw_height - inset.top - inset.height,
            },
        };
    }

    const auto used_width = input.legacy_left + input.legacy_width;
    const auto used_height = input.legacy_top + input.legacy_height;
    return {
        Dimensions{input.legacy_width, input.legacy_height},
        Margins{
            input.legacy_left,
            input.legacy_top,
            input.raw_width > used_width ? input.raw_width - used_width : 0U,
            input.raw_height > used_height ? input.raw_height - used_height : 0U,
        },
    };
}

} // namespace shadow::image::detail
