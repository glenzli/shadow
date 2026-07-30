#pragma once

#include "backend/edit_types.hpp"

#include <QPointF>

#include <optional>

// Exact presentation-to-authoring coordinate boundary for Liquify. The
// preview is post-Canvas; persisted deformation points are pre-Canvas in the
// uncropped original-image space.
namespace EditLiquifyCoordinates {

[[nodiscard]] std::optional<QPointF> originalPointForOutput(
    QPointF output,
    double output_aspect_ratio,
    const BackendPhotoGeometry& geometry
);

} // namespace EditLiquifyCoordinates
