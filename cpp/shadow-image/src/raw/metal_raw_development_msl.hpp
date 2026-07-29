#pragma once

#include "metal_dcp_color_msl.hpp"
#include "metal_raw_common_msl.hpp"
#include "metal_raw_denoise_msl.hpp"
#include "metal_raw_reconstruction_msl.hpp"

#include <array>
#include <string>
#include <string_view>

namespace shadow::image::detail {

// Compose responsibility-owned fragments into one source string so the runtime still creates one
// MTLLibrary and all RAW stages can remain in a single resident command transaction.
[[nodiscard]] inline std::string metal_raw_kernel_source() {
    constexpr std::array<std::string_view, 4U> fragments{
        metal_raw_common_source,
        metal_raw_denoise_source,
        metal_raw_reconstruction_source,
        metal_dcp_color_source,
    };
    std::size_t total_size = 0U;
    for (const auto fragment : fragments) {
        total_size += fragment.size();
    }

    std::string source;
    source.reserve(total_size);
    for (const auto fragment : fragments) {
        source.append(fragment);
    }
    return source;
}

} // namespace shadow::image::detail
