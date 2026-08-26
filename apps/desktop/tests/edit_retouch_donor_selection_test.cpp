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

[[nodiscard]] std::vector<std::uint8_t> photographic_texture(const QSize dimensions) {
    const std::size_t width = static_cast<std::size_t>(dimensions.width());
    const std::size_t height = static_cast<std::size_t>(dimensions.height());
    std::vector<std::uint8_t> pixels(width * height * 3U, 0U);
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            const double wave = 34.0 * std::sin(static_cast<double>(x) * 0.31)
                                + 27.0 * std::cos(static_cast<double>(y) * 0.19)
                                + 18.0 * std::sin(static_cast<double>(x + 2U * y) * 0.11);
            const int base = std::clamp(
                static_cast<int>(std::lround(112.0 + 0.23 * static_cast<double>(x) + wave)),
                0,
                255
            );
            const std::size_t index = (y * width + x) * 3U;
            pixels[index] = static_cast<std::uint8_t>(base);
            pixels[index + 1U] = static_cast<std::uint8_t>(std::clamp(base + 17, 0, 255));
            pixels[index + 2U] = static_cast<std::uint8_t>(std::clamp(228 - base / 2, 0, 255));
        }
    }
    return pixels;
}

void copy_patch(
    std::vector<std::uint8_t>& pixels,
    const QSize dimensions,
    const int source_center_x,
    const int source_center_y,
    const int destination_center_x,
    const int destination_center_y,
    const int half_extent
) {
    const std::vector<std::uint8_t> original = pixels;
    const std::size_t width = static_cast<std::size_t>(dimensions.width());
    for (int offset_y = -half_extent; offset_y <= half_extent; ++offset_y) {
        for (int offset_x = -half_extent; offset_x <= half_extent; ++offset_x) {
            const std::size_t source = (static_cast<std::size_t>(source_center_y + offset_y) * width
                                        + static_cast<std::size_t>(source_center_x + offset_x))
                                       * 3U;
            const std::size_t destination =
                (static_cast<std::size_t>(destination_center_y + offset_y) * width
                 + static_cast<std::size_t>(destination_center_x + offset_x))
                * 3U;
            std::copy_n(
                original.begin() + static_cast<std::ptrdiff_t>(source),
                3,
                pixels.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
}

[[nodiscard]] EditRetouchDonorRequest request(
    const std::span<const std::uint8_t> pixels,
    const QSize dimensions,
    const std::span<const QPointF> points,
    const EditRetouchDonorMode mode = EditRetouchDonorMode::Clone
) {
    return {
        .preview_rgb8 = pixels,
        .preview_dimensions = dimensions,
        .preview_row_stride_bytes = static_cast<std::size_t>(dimensions.width()) * 3U,
        .level_zero_dimensions = dimensions,
        .normalized_target_points = points,
        .radius_level_zero_pixels = 10.0,
        .mode = mode,
    };
}

} // namespace

int main() {
    const QSize dimensions(320, 200);
    const std::vector<std::uint8_t> pixels = vertical_gradient(dimensions);
    const std::array<QPointF, 1U> centered{QPointF(0.5, 0.5)};
    const auto centered_offset = select_edit_retouch_donor(request(pixels, dimensions, centered));
    if (!require(centered_offset.has_value(), "a valid preview selects a donor")
        || !require(
            std::abs(centered_offset->offset_radii.x() - 2.5) < 1.0e-9
                && std::abs(centered_offset->offset_radii.y()) < 1.0e-9,
            "selection follows matching texture rather than changing gradient bands"
        )
        || !require(
            centered_offset->confidence > 0.7 && centered_offset->candidate_count > 1U,
            "an equivalent smooth donor reports useful automatic-source confidence"
        )) {
        return EXIT_FAILURE;
    }

    const std::array<QPointF, 1U> near_right_edge{QPointF(0.94, 0.5)};
    const auto edge_offset =
        select_edit_retouch_donor(request(pixels, dimensions, near_right_edge));
    if (!require(edge_offset.has_value(), "an edge target still finds an in-bounds donor")
        || !require(
            edge_offset->offset_radii.x() < 0.0 && std::abs(edge_offset->offset_radii.y()) < 1.0e-9,
            "edge selection rejects clamped source pixels and searches inward"
        )) {
        return EXIT_FAILURE;
    }

    std::vector<std::uint8_t> textured = photographic_texture(dimensions);
    constexpr int target_x = 112;
    constexpr int target_y = 100;
    constexpr int donor_x = target_x + 50;
    copy_patch(textured, dimensions, target_x, target_y, donor_x, target_y, 30);
    const std::array<QPointF, 1U> textured_target{QPointF(
        static_cast<double>(target_x) / static_cast<double>(dimensions.width() - 1),
        static_cast<double>(target_y) / static_cast<double>(dimensions.height() - 1)
    )};
    const auto texture_clone = select_edit_retouch_donor(
        request(textured, dimensions, textured_target, EditRetouchDonorMode::Clone)
    );
    const auto texture_heal = select_edit_retouch_donor(
        request(textured, dimensions, textured_target, EditRetouchDonorMode::Heal)
    );
    if (!require(
            texture_clone.has_value() && texture_heal.has_value(),
            "multiscale texture fixtures select a donor for both modes"
        )
        || !require(
            std::abs(texture_clone->offset_radii.x() - 5.0) < 1.0e-9
                && std::abs(texture_clone->offset_radii.y()) < 1.0e-9
                && std::abs(texture_heal->offset_radii.x() - 5.0) < 1.0e-9
                && std::abs(texture_heal->offset_radii.y()) < 1.0e-9,
            "multiscale color and directed-gradient structure recover the copied texture"
        )) {
        return EXIT_FAILURE;
    }

    const std::array<QPointF, 2U> horizontal_stroke{
        QPointF(0.4, 0.5),
        QPointF(0.55, 0.5),
    };
    const auto separated_stroke =
        select_edit_retouch_donor(request(pixels, dimensions, horizontal_stroke));
    if (!require(separated_stroke.has_value(), "a long stroke finds a non-overlapping donor")) {
        return EXIT_FAILURE;
    }
    const double stroke_length_radii = 0.15 * static_cast<double>(dimensions.width() - 1) / 10.0;
    const double minimum_separation = 2.15;
    const double gap_x =
        std::max(0.0, std::abs(separated_stroke->offset_radii.x()) - stroke_length_radii);
    const double gap_y = std::abs(separated_stroke->offset_radii.y());
    if (!require(
            std::hypot(gap_x, gap_y) >= minimum_separation - 1.0e-9,
            "donor selection rejects source strokes that overlap their target sweep"
        )) {
        return EXIT_FAILURE;
    }

    EditRetouchDonorRequest malformed = request(pixels, dimensions, centered);
    malformed.preview_row_stride_bytes = 1U;
    if (!require(
            !select_edit_retouch_donor(malformed).has_value(),
            "malformed preview storage fails closed"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
