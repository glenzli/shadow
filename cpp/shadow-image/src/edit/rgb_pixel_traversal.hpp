#pragma once

#include "adjustment_node_diagnostics.hpp"
#include "working_color_math.hpp"
#include "../concurrency/row_scheduler.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace shadow::image::detail {

// Owns the common, node-attributed traversal contract for pixel-local RGB
// transforms. Algorithms remain responsible for their color semantics; this
// boundary owns only row scheduling, interleaved-RGB addressing, and checked
// writes.
template <typename Transform>
void transform_rgb_pixels(FloatRgbImage& image, const std::size_t node_index,
                          const AdjustmentNode& node, Transform&& transform) {
    constexpr std::size_t rgb_channels = 3U;
    constexpr std::uint32_t minimum_rows_per_chunk = 32U;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    parallel_for_rows(
        image.dimensions.height, minimum_rows_per_chunk,
        [&image, stride, node_index, &node, transform = std::forward<Transform>(transform)](
            const std::uint32_t first_row, const std::uint32_t past_last_row) mutable {
            for (std::uint32_t y = first_row; y < past_last_row; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * stride;
                for (std::uint32_t x = 0; x < image.dimensions.width; ++x) {
                    const std::size_t sample = row + static_cast<std::size_t>(x) * rgb_channels;
                    const Vector3 input{
                        static_cast<double>(image.samples[sample]),
                        static_cast<double>(image.samples[sample + 1U]),
                        static_cast<double>(image.samples[sample + 2U]),
                    };
                    const Vector3 output = transform(input);
                    image.samples[sample] =
                        checked_edit_pixel_float(output[0], node_index, node);
                    image.samples[sample + 1U] =
                        checked_edit_pixel_float(output[1], node_index, node);
                    image.samples[sample + 2U] =
                        checked_edit_pixel_float(output[2], node_index, node);
                }
            }
        });
}

} // namespace shadow::image::detail
