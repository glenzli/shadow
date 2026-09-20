#include "edit_retouch_sources.hpp"
#include "edit_controller.hpp"
#include "edit_retouch_donor_preview.hpp"
#include <QUrlQuery>
#include <algorithm>
#include <cmath>

EditRetouchSources::EditRetouchSources(EditController& owner) : QObject(&owner), owner_(owner) {
    connect(&owner_, &EditController::sourceIdentityChanged, this, [this] {
        saved_.clear();
        candidates_.clear();
        basis_.reset();
        status_.clear();
        saved_geometry_ = owner_.grade_stack_.geometry;
        emit changed();
    });
    connect(&owner_, &EditController::parametersChanged, this, [this] {
        if (saved_geometry_ != owner_.grade_stack_.geometry) {
            saved_.clear();
            candidates_.clear();
            basis_.reset();
            status_.clear();
            saved_geometry_ = owner_.grade_stack_.geometry;
            owner_.clearRetouchSource();
            emit changed();
        }
    });
}
QVariantList EditRetouchSources::saved() const {
    QVariantList values;
    for (int i = 0; i < 3; ++i)
        values.push_back(i < int(saved_.size()));
    return values;
}
void EditRetouchSources::setPreviewOpacity(double value) {
    if (!std::isfinite(value))
        return;
    value = std::clamp(value, 0.1, 1.0);
    if (preview_opacity_ == value)
        return;
    preview_opacity_ = value;
    emit changed();
}
void EditRetouchSources::remember() {
    if (!owner_.active_ || !owner_.retouch_source_anchor_)
        return;
    const auto point = *owner_.retouch_source_anchor_;
    saved_.erase(std::remove(saved_.begin(), saved_.end(), point), saved_.end());
    saved_.insert(saved_.begin(), point);
    if (saved_.size() > 3)
        saved_.resize(3);
    saved_geometry_ = owner_.grade_stack_.geometry;
    status_ = tr("Source remembered for this photo and framing.");
    emit changed();
}
void EditRetouchSources::recall(int index) {
    if (!owner_.active_ || owner_.interactionLocked() || index < 0 || index >= int(saved_.size()))
        return;
    owner_.setRetouchPickerActive(true);
    owner_.setRetouchSourceFromPreview(saved_[index].x(), saved_[index].y());
    status_.clear();
    emit changed();
}
QVariantMap EditRetouchSources::previewSourceAt(double x, double y) const {
    if (!owner_.retouch_source_anchor_)
        return {};
    QPointF source = *owner_.retouch_source_anchor_;
    if (owner_.retouch_source_aligned_ && owner_.retouch_aligned_source_offset_radii_
        && owner_.level_zero_width_ && owner_.level_zero_height_) {
        const auto offset = *owner_.retouch_aligned_source_offset_radii_;
        source = {
            x + offset.x() * owner_.retouch_brush_radius_ / owner_.level_zero_width_,
            y + offset.y() * owner_.retouch_brush_radius_ / owner_.level_zero_height_
        };
    }
    return {{"x", source.x()}, {"y", source.y()}};
}
void EditRetouchSources::nextCandidate(bool continuous, int index) {
    if (!owner_.active_ || owner_.interactionLocked() || index < 0)
        return;
    auto basis = owner_.grade_stack_;
    std::vector<QPointF> points;
    double radius = 0;
    int mode = 0;
    QPointF current;
    if (continuous) {
        if (index >= basis.retouch_strokes.size())
            return;
        auto& stroke = basis.retouch_strokes[index];
        if (stroke.source_rotation_degrees != 0 || stroke.source_scale != 1
            || stroke.source_flip_horizontal || stroke.source_flip_vertical)
            return;
        for (const auto& point : stroke.points)
            points.emplace_back(point.x, point.y);
        radius = stroke.radius_level_zero_pixels;
        mode = stroke.mode;
        current = {stroke.source_offset_x_radii, stroke.source_offset_y_radii};
        stroke.source_offset_x_radii = 0;
        stroke.source_offset_y_radii = 0;
    } else {
        if (index >= basis.retouch_spots.size())
            return;
        auto& spot = basis.retouch_spots[index];
        if (spot.source_rotation_degrees != 0 || spot.source_scale != 1
            || spot.source_flip_horizontal || spot.source_flip_vertical)
            return;
        points.emplace_back(spot.center_x, spot.center_y);
        radius = spot.radius_level_zero_pixels;
        mode = spot.mode;
        current = {spot.source_offset_x_radii, spot.source_offset_y_radii};
        spot.source_offset_x_radii = 0;
        spot.source_offset_y_radii = 0;
    }
    if (!basis_ || *basis_ != basis || candidate_index_ != index || continuous_ != continuous) {
        const auto generation =
            QUrlQuery(QUrl(owner_.previewSource())).queryItemValue("generation");
        const auto selection = preview_retouch_source_selection(
            owner_.preview_store_,
            generation,
            QSize(int(owner_.level_zero_width_), int(owner_.level_zero_height_)),
            points,
            radius,
            mode
        );
        if (!selection || selection->ranked_offsets.empty()) {
            status_ = tr("No suitable nearby source. Drag the source outline to choose one.");
            emit changed();
            return;
        }
        candidates_ = selection->ranked_offsets;
        basis_ = basis;
        candidate_index_ = index;
        continuous_ = continuous;
    }
    auto it = std::find_if(candidates_.begin(), candidates_.end(), [&](QPointF p) {
        return std::hypot(p.x() - current.x(), p.y() - current.y()) < 1e-5;
    });
    const int next = it == candidates_.end() ? 0
                                             : (int(std::distance(candidates_.begin(), it)) + 1)
                                                   % int(candidates_.size());
    const auto offset = candidates_[next];
    if (continuous)
        owner_.setRetouchStrokeSourceOffset(index, offset.x(), offset.y());
    else
        owner_.setRetouchSpotSourceOffset(index, offset.x(), offset.y());
    status_ = tr("Source %1 of %2 · undo restores the previous source")
                  .arg(next + 1)
                  .arg(candidates_.size());
    emit changed();
}
