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
        saved_liquify_ = owner_.grade_stack_.liquify_strokes;
        saved_liquify_enabled_ = owner_.grade_stack_.liquify_enabled;
        emit changed();
    });
    connect(&owner_, &EditController::parametersChanged, this, [this] {
        if (saved_geometry_ != owner_.grade_stack_.geometry
            || saved_liquify_ != owner_.grade_stack_.liquify_strokes
            || saved_liquify_enabled_ != owner_.grade_stack_.liquify_enabled) {
            saved_.clear();
            candidates_.clear();
            basis_.reset();
            status_.clear();
            saved_geometry_ = owner_.grade_stack_.geometry;
            saved_liquify_ = owner_.grade_stack_.liquify_strokes;
            saved_liquify_enabled_ = owner_.grade_stack_.liquify_enabled;
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
    const auto p = coordinates().preview(saved_[static_cast<std::size_t>(index)]);
    if (!p)
        return;
    owner_.setRetouchSourceFromPreview(p->x(), p->y());
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
        const auto target = coordinates().original({x, y});
        if (!target)
            return {};
        source = {
            target->x() + offset.x() * owner_.retouch_brush_radius_ / owner_.level_zero_width_,
            target->y() + offset.y() * owner_.retouch_brush_radius_ / owner_.level_zero_height_
        };
    }
    const auto projected = coordinates().preview(source);
    return projected ? QVariantMap{{"x", projected->x()}, {"y", projected->y()}} : QVariantMap{};
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
        const auto selection = selectSource(generation, points, radius, mode);
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
    const auto offset = candidates_[static_cast<std::size_t>(next)];
    if (continuous)
        owner_.setRetouchStrokeSourceOffset(index, offset.x(), offset.y());
    else
        owner_.setRetouchSpotSourceOffset(index, offset.x(), offset.y());
    status_ = tr("Source %1 of %2 · undo restores the previous source")
                  .arg(next + 1)
                  .arg(candidates_.size());
    emit changed();
}

std::optional<EditRetouchDonorSelection> EditRetouchSources::selectSource(
    const QString& generation,
    std::span<const QPointF> points,
    double radius,
    int mode
) const {
    if (points.empty())
        return std::nullopt;
    const auto& map = coordinates();
    std::vector<QPointF> projected;
    double minX = 1, maxX = 0, minY = 1, maxY = 0, viewMinX = 1, viewMaxX = 0, viewMinY = 1,
           viewMaxY = 0;
    for (auto p : points) {
        const auto q = map.preview(p);
        if (!q)
            return std::nullopt;
        projected.push_back(*q);
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
        viewMinX = std::min(viewMinX, q->x());
        viewMaxX = std::max(viewMaxX, q->x());
        viewMinY = std::min(viewMinY, q->y());
        viewMaxY = std::max(viewMaxY, q->y());
    }
    auto result = preview_retouch_source_selection(
        owner_.preview_store_,
        generation,
        map.outputSize(),
        projected,
        radius,
        mode
    );
    if (!result)
        return std::nullopt;
    const QPointF original((minX + maxX) / 2, (minY + maxY) / 2),
        view((viewMinX + viewMaxX) / 2, (viewMinY + viewMaxY) / 2);
    const auto convert = [&](QPointF offset) -> std::optional<QPointF> {
        const auto donor = map.original(
            view
            + QPointF(
                offset.x() * radius / map.outputSize().width(),
                offset.y() * radius / map.outputSize().height()
            )
        );
        if (!donor)
            return std::nullopt;
        const QPointF value(
            (donor->x() - original.x()) * map.sourceSize().width() / radius,
            (donor->y() - original.y()) * map.sourceSize().height() / radius
        );
        const double limit = 511 / radius - 1;
        if (std::abs(value.x()) > limit || std::abs(value.y()) > limit)
            return std::nullopt;
        return value;
    };
    const auto selected = convert(result->offset_radii);
    if (!selected)
        return std::nullopt;
    result->offset_radii = *selected;
    std::vector<QPointF> ranked;
    for (auto p : result->ranked_offsets)
        if (const auto value = convert(p))
            ranked.push_back(*value);
    result->ranked_offsets = std::move(ranked);
    return result;
}

void EditRetouchSources::setFrequencyRadius(int value) {
    if (value < 2 || value > 32 || value == frequency_radius_)
        return;
    frequency_radius_ = value;
    emit changed();
}
void EditRetouchSources::setRegionFrequency(bool continuous, int index, int value) {
    if (!owner_.active_ || owner_.interactionLocked() || value < 2 || value > 32 || index < 0)
        return;
    if (continuous ? index >= owner_.grade_stack_.retouch_strokes.size()
                   : index >= owner_.grade_stack_.retouch_spots.size())
        return;
    const auto before = owner_.grade_stack_;
    auto& radius = continuous ? owner_.grade_stack_.retouch_strokes[index].frequency_radius
                              : owner_.grade_stack_.retouch_spots[index].frequency_radius;
    if (radius == value)
        return;
    radius = static_cast<std::uint16_t>(value);
    owner_.parameterEdited(
        continuous ? QStringLiteral("retouch/stroke/%1/frequency").arg(index)
                   : QStringLiteral("retouch/%1/frequency").arg(index),
        before
    );
}
