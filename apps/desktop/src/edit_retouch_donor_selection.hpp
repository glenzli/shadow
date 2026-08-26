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

struct EditRetouchDonorSelection final {
    QPointF offset_radii;
    // Zero means the sampled boundary has no convincing nearby match; one is
    // an exact and meaningfully preferred match. The score is presentation
    // guidance only: the selected offset remains deterministic Recipe input.
    double confidence = 0.0;
    std::size_t candidate_count = 0U;
};

// Selects one source displacement in brush-radius units and reports how well
// the target boundary is explained by that nearby source. The offset is stored
// in the existing Recipe fields, so every later preview, detail render, and
// export consumes the same decision without re-running analysis.
[[nodiscard]] std::optional<EditRetouchDonorSelection>
select_edit_retouch_donor(const EditRetouchDonorRequest& request);
