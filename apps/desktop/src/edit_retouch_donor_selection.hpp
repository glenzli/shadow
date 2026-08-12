#pragma once

#include <QPointF>
#include <QSize>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

enum class EditRetouchDonorMode : std::uint8_t {
    Heal,
    Clone,
};

struct EditRetouchDonorRequest final {
    std::span<const std::uint8_t> preview_rgb8;
    QSize preview_dimensions;
    std::size_t preview_row_stride_bytes = 0U;
    QSize level_zero_dimensions;
    std::span<const QPointF> normalized_target_points;
    double radius_level_zero_pixels = 0.0;
    EditRetouchDonorMode mode = EditRetouchDonorMode::Heal;
};

// Selects one source displacement in brush-radius units. The result is stored
// in the existing Recipe source-offset fields, so every later preview, detail
// render, and export consumes the same decision without re-running analysis.
[[nodiscard]] std::optional<QPointF>
select_edit_retouch_donor_offset(const EditRetouchDonorRequest& request);
