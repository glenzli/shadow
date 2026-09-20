#include "edit_controller.hpp"
#include "edit_retouch_sources.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
template <typename Region> QVariantMap regionParameters(const Region& region) {
    return {
        {"radius", region.radius_level_zero_pixels},
        {"sourceScale", region.source_scale},
        {"sourceRotation", region.source_rotation_degrees},
        {"sourceFlipHorizontal", region.source_flip_horizontal},
        {"sourceFlipVertical", region.source_flip_vertical},
        {"sourceOffsetX", region.source_offset_x_radii},
        {"sourceOffsetY", region.source_offset_y_radii}
    };
}
} // namespace

const EditRetouchCoordinates& EditRetouchSources::coordinates() const {
    coordinates_.update(
        owner_.grade_stack_,
        {int(owner_.level_zero_width_), int(owner_.level_zero_height_)}
    );
    return coordinates_;
}
bool EditRetouchSources::projectionRequired() const {
    auto geometry = owner_.grade_stack_.geometry;
    geometry.enabled = true;
    auto identity = BackendPhotoGeometry{};
    identity.enabled = true;
    return (owner_.grade_stack_.geometry.enabled && geometry != identity)
           || (owner_.grade_stack_.liquify_enabled
               && !owner_.grade_stack_.liquify_strokes.isEmpty());
}
double EditRetouchSources::previewRadius(double x, double y) const {
    const auto& map = coordinates();
    const auto p = map.original({x, y});
    if (!p || map.sourceSize().width() <= 0)
        return 0;
    const auto q = map.preview(
        *p + QPointF(double(owner_.retouch_brush_radius_) / map.sourceSize().width(), 0)
    );
    if (!q)
        return 0;
    return std::hypot(
        (q->x() - x),
        (q->y() - y) * map.outputSize().height() / map.outputSize().width()
    );
}
QVariantMap EditRetouchSources::projectRegion(bool continuous, int index, bool detailed) const {
    if (index < 0
        || (continuous ? index >= owner_.grade_stack_.retouch_strokes.size()
                       : index >= owner_.grade_stack_.retouch_spots.size()))
        return {};
    auto value = continuous ? regionParameters(owner_.grade_stack_.retouch_strokes[index])
                            : regionParameters(owner_.grade_stack_.retouch_spots[index]);
    // Only legacy implicit donors need the automatic-source presentation path;
    // ordinary persisted donors never serialize every other region to project one.
    if (value["sourceOffsetX"].toDouble() == 0 && value["sourceOffsetY"].toDouble() == 0)
        value = (continuous ? owner_.retouchStrokes() : owner_.retouchSpots())[index].toMap();
    QVector<QPointF> points;
    if (continuous) {
        for (const auto& p : owner_.grade_stack_.retouch_strokes[index].points)
            points.push_back({p.x, p.y});
    } else {
        const auto& p = owner_.grade_stack_.retouch_spots[index];
        points.push_back({p.center_x, p.center_y});
    }
    if (points.isEmpty())
        return {};
    const auto& map = coordinates();
    const auto size = map.sourceSize();
    if (size.width() <= 0 || size.height() <= 0)
        return {};
    double minX = 1, maxX = 0, minY = 1, maxY = 0;
    for (auto p : points) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }
    const QPointF anchor((minX + maxX) / 2, (minY + maxY) / 2);
    const double radius = value["radius"].toDouble(), scale = value["sourceScale"].toDouble();
    const double angle = value["sourceRotation"].toDouble() * std::numbers::pi / 180;
    const double c = std::cos(angle), s = std::sin(angle);
    const double fx = value["sourceFlipHorizontal"].toBool() ? -1 : 1;
    const double fy = value["sourceFlipVertical"].toBool() ? -1 : 1;
    QVector<QPointF> donors;
    double loX = 1e30, hiX = -1e30, loY = 1e30, hiY = -1e30;
    const double haloX = radius * scale / size.width(), haloY = radius * scale / size.height();
    for (auto p : points) {
        const double x = (p.x() - anchor.x()) * size.width() * fx,
                     y = (p.y() - anchor.y()) * size.height() * fy;
        QPointF q(
            anchor.x() + scale * (c * x - s * y) / size.width(),
            anchor.y() + scale * (s * x + c * y) / size.height()
        );
        donors.push_back(q);
        loX = std::min(loX, q.x() - haloX);
        hiX = std::max(hiX, q.x() + haloX);
        loY = std::min(loY, q.y() - haloY);
        hiY = std::max(hiY, q.y() + haloY);
    }
    const auto bounded = [](double v, double lo, double hi) {
        return lo <= hi ? std::clamp(v, lo, hi) : (lo + hi) / 2;
    };
    const QPointF offset(
        bounded(value["sourceOffsetX"].toDouble() * radius / size.width(), -loX, 1 - hiX),
        bounded(value["sourceOffsetY"].toDouble() * radius / size.height(), -loY, 1 - hiY)
    );
    QVariantList target, source;
    const auto present = [&](QPointF p, double r) -> QVariantMap {
        const auto q = map.preview(p), edgeX = map.preview(p + QPointF(r / size.width(), 0)),
                   edgeY = map.preview(p + QPointF(0, r / size.height()));
        if (!q || !edgeX || !edgeY)
            return {};
        return {
            {"x", q->x()},
            {"y", q->y()},
            {"ux", edgeX->x() - q->x()},
            {"uy", edgeX->y() - q->y()},
            {"vx", edgeY->x() - q->x()},
            {"vy", edgeY->y() - q->y()}
        };
    };
    if (!detailed) {
        const auto t = present(anchor, radius), d = present(anchor + offset, radius * scale);
        if (t.isEmpty() || d.isEmpty())
            return {};
        target.push_back(t);
        source.push_back(d);
        return {{"target", target}, {"source", source}};
    }
    for (qsizetype i = 0; i < points.size(); ++i) {
        const auto t = present(points[i], radius), d = present(donors[i] + offset, radius * scale);
        if (t.isEmpty() || d.isEmpty())
            return {};
        target.push_back(t);
        source.push_back(d);
    }
    return {{"target", target}, {"source", source}};
}
void EditRetouchSources::moveRegion(
    bool continuous,
    int index,
    bool source,
    double fromX,
    double fromY,
    double toX,
    double toY
) {
    if (!owner_.active_ || owner_.interactionLocked() || index < 0
        || (continuous ? index >= owner_.grade_stack_.retouch_strokes.size()
                       : index >= owner_.grade_stack_.retouch_spots.size()))
        return;
    const auto& map = coordinates();
    const auto from = map.original({fromX, fromY}), to = map.original({toX, toY});
    if (!from || !to)
        return;
    const auto delta = *to - *from;
    if (source) {
        auto value = continuous ? regionParameters(owner_.grade_stack_.retouch_strokes[index])
                                : regionParameters(owner_.grade_stack_.retouch_spots[index]);
        if (value["sourceOffsetX"].toDouble() == 0 && value["sourceOffsetY"].toDouble() == 0)
            value = (continuous ? owner_.retouchStrokes() : owner_.retouchSpots())[index].toMap();
        const double radius = value["radius"].toDouble();
        const double x =
            value["sourceOffsetX"].toDouble() + delta.x() * map.sourceSize().width() / radius;
        const double y =
            value["sourceOffsetY"].toDouble() + delta.y() * map.sourceSize().height() / radius;
        if (continuous)
            owner_.setRetouchStrokeSourceOffset(index, x, y);
        else
            owner_.setRetouchSpotSourceOffset(index, x, y);
    } else if (continuous)
        owner_.translateRetouchStroke(index, delta.x(), delta.y());
    else {
        const auto p = owner_.grade_stack_.retouch_spots[index];
        owner_.setRetouchSpotCenter(
            index,
            std::clamp(p.center_x + delta.x(), 0.0, 1.0),
            std::clamp(p.center_y + delta.y(), 0.0, 1.0)
        );
    }
}
