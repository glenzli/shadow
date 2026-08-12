#include "edit_retouch_donor_selection.hpp"

#include <QPointF>
#include <QSize>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] std::vector<std::uint8_t> vertical_gradient(const QSize dimensions) {
    const std::size_t width = static_cast<std::size_t>(dimensions.width());
    const std::size_t height = static_cast<std::size_t>(dimensions.height());
    std::vector<std::uint8_t> pixels(width * height * 3U, 0U);
    for (std::size_t y = 0U; y < height; ++y) {
        const auto value =
            static_cast<std::uint8_t>(32U + (y * 180U) / std::max<std::size_t>(1U, height - 1U));
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t index = (y * width + x) * 3U;
            pixels[index] = value;
            pixels[index + 1U] = static_cast<std::uint8_t>(value / 2U);
            pixels[index + 2U] = static_cast<std::uint8_t>(255U - value);
        }
    }
    return pixels;
}

[[nodiscard]] EditRetouchDonorRequest request(
    const std::span<const std::uint8_t> pixels,
    const QSize dimensions,
    const std::span<const QPointF> points
) {
    return {
        .preview_rgb8 = pixels,
        .preview_dimensions = dimensions,
        .preview_row_stride_bytes = static_cast<std::size_t>(dimensions.width()) * 3U,
        .level_zero_dimensions = dimensions,
        .normalized_target_points = points,
        .radius_level_zero_pixels = 10.0,
        .mode = EditRetouchDonorMode::Clone,
    };
}

} // namespace

int main() {
    const QSize dimensions(320, 200);
    const std::vector<std::uint8_t> pixels = vertical_gradient(dimensions);
    const std::array<QPointF, 1U> centered{QPointF(0.5, 0.5)};
    const auto centered_offset =
        select_edit_retouch_donor_offset(request(pixels, dimensions, centered));
    if (!require(centered_offset.has_value(), "a valid preview selects a donor")
        || !require(
            std::abs(centered_offset->x() - 2.5) < 1.0e-9
                && std::abs(centered_offset->y()) < 1.0e-9,
            "selection follows matching texture rather than changing gradient bands"
        )) {
        return EXIT_FAILURE;
    }

    const std::array<QPointF, 1U> near_right_edge{QPointF(0.94, 0.5)};
    const auto edge_offset =
        select_edit_retouch_donor_offset(request(pixels, dimensions, near_right_edge));
    if (!require(edge_offset.has_value(), "an edge target still finds an in-bounds donor")
        || !require(
            edge_offset->x() < 0.0 && std::abs(edge_offset->y()) < 1.0e-9,
            "edge selection rejects clamped source pixels and searches inward"
        )) {
        return EXIT_FAILURE;
    }

    EditRetouchDonorRequest malformed = request(pixels, dimensions, centered);
    malformed.preview_row_stride_bytes = 1U;
    if (!require(
            !select_edit_retouch_donor_offset(malformed).has_value(),
            "malformed preview storage fails closed"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
