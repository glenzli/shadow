#pragma once
#include "backend/edit_types.hpp"
#include <QPointF>
#include <QSize>
#include <QTransform>
#include <optional>
#include <shadow/image/photo_liquify.hpp>

// Coordinate-only cache. Rebuilt for source extent or structural edits, never
// for a retouch parameter. No image readback, Recipe migration or render work.
class EditRetouchCoordinates final {
  public:
    bool update(const BackendGradeStack&, QSize);
    std::optional<QPointF> original(QPointF) const;
    std::optional<QPointF> preview(QPointF) const;
    QSize outputSize() const {
        return output_size_;
    }
    QSize sourceSize() const {
        return source_size_;
    }

  private:
    QSize source_size_, output_size_;
    BackendPhotoGeometry geometry_;
    QVector<BackendLiquifyStroke> strokes_;
    shadow::image::PreparedPhotoLiquify liquify_;
    QTransform to_original_, to_preview_;
    bool ready_ = false;
};
