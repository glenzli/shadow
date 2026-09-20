#include "edit_retouch_coordinates.hpp"
#include <QPolygonF>
#include <algorithm>
#include <cmath>
#include <shadow/image/photo_geometry.hpp>

bool EditRetouchCoordinates::update(const BackendGradeStack& stack, QSize size) {
    const auto geometry = stack.geometry.enabled ? stack.geometry : BackendPhotoGeometry{};
    const auto& strokes = stack.liquify_strokes;
    const bool deformed = stack.liquify_enabled && !strokes.isEmpty();
    if (ready_ && size == source_size_ && geometry == geometry_
        && (deformed ? strokes == strokes_ : strokes_.isEmpty()))
        return true;
    ready_ = false;
    if (size.width() <= 0 || size.height() <= 0)
        return false;
    const shadow::image::Dimensions extent{
        std::uint32_t(size.width()),
        std::uint32_t(size.height())
    };
    try {
        shadow::image::PhotoGeometry native{
            .crop_left = geometry.crop_left,
            .crop_top = geometry.crop_top,
            .crop_right = geometry.crop_right,
            .crop_bottom = geometry.crop_bottom,
            .quarter_turn = static_cast<shadow::image::PhotoQuarterTurn>(geometry.quarter_turn),
            .straighten_degrees = geometry.straighten_degrees,
            .perspective_vertical = geometry.perspective_vertical,
            .perspective_horizontal = geometry.perspective_horizontal,
            .flip_horizontal = geometry.flip_horizontal,
            .flip_vertical = geometry.flip_vertical
        };
        const auto layout = shadow::image::photo_geometry_layout(extent, native);
        output_size_ = {int(layout.output_dimensions.width), int(layout.output_dimensions.height)};
        const QPolygonF output{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        QPolygonF input;
        for (auto p : output) {
            const auto q = shadow::image::photo_geometry_source_point(extent, native, p.x(), p.y());
            input << QPointF(q[0], q[1]);
        }
        if (!QTransform::quadToQuad(output, input, to_original_))
            return false;
        bool invertible = false;
        to_preview_ = to_original_.inverted(&invertible);
        if (!invertible)
            return false;
        liquify_ = {};
        if (deformed) {
            shadow::image::PhotoLiquify native_liquify;
            for (const auto& s : strokes) {
                std::vector<shadow::image::PhotoLiquifyPoint> points;
                for (const auto& p : s.points)
                    points.push_back({p.x, p.y, p.pressure});
                if (s.kind == 1)
                    native_liquify.strokes.push_back(
                        shadow::image::PhotoLiquifyReconstructStroke{
                            std::move(points),
                            s.radius,
                            s.strength,
                            s.hardness
                        }
                    );
                else
                    native_liquify.strokes.push_back(
                        shadow::image::PhotoLiquifyPushStroke{
                            std::move(points),
                            s.radius,
                            s.strength,
                            s.hardness
                        }
                    );
            }
            liquify_ = shadow::image::prepare_photo_liquify(extent, native_liquify);
        }
    } catch (const std::exception&) {
        return false;
    }
    source_size_ = size;
    geometry_ = geometry;
    strokes_ = deformed ? strokes : QVector<BackendLiquifyStroke>{};
    ready_ = true;
    return true;
}

std::optional<QPointF> EditRetouchCoordinates::original(QPointF p) const {
    if (!ready_ || !std::isfinite(p.x()) || !std::isfinite(p.y()))
        return std::nullopt;
    p = to_original_.map(p);
    const auto q = shadow::image::photo_liquify_source_point(liquify_, {p.x(), p.y(), 1});
    if (!std::isfinite(q.x) || !std::isfinite(q.y))
        return std::nullopt;
    return QPointF(q.x, q.y);
}

std::optional<QPointF> EditRetouchCoordinates::preview(QPointF p) const {
    if (!ready_ || !std::isfinite(p.x()) || !std::isfinite(p.y()))
        return std::nullopt;
    QPointF q = p;
    const auto sample = [&](QPointF v) {
        const auto s = shadow::image::photo_liquify_source_point(liquify_, {v.x(), v.y(), 1});
        return QPointF(s.x, s.y);
    };
    // Invert the same sampler used by export. Bounded Newton iterations are
    // coordinate work only; a folded/noninvertible region has no false handle.
    if (!liquify_.stamps.empty()) {
        const double tolerance = 0.02 / std::max(source_size_.width(), source_size_.height());
        bool converged = false;
        for (int i = 0; i < 16; ++i) {
            const auto a = sample(q), error = a - p;
            if (std::hypot(error.x(), error.y()) < tolerance) {
                converged = true;
                break;
            }
            constexpr double h = 1e-5;
            const auto dx = (sample(q + QPointF(h, 0)) - a) / h;
            const auto dy = (sample(q + QPointF(0, h)) - a) / h;
            const double det = dx.x() * dy.y() - dy.x() * dx.y();
            if (!std::isfinite(det) || std::abs(det) < 1e-8)
                return std::nullopt;
            const QPointF step(
                (dy.y() * error.x() - dy.x() * error.y()) / det,
                (dx.x() * error.y() - dx.y() * error.x()) / det
            );
            double damping = 1;
            for (int j = 0; j < 6; ++j) {
                const auto next = sample(q - step * damping) - p;
                if (std::hypot(next.x(), next.y()) < std::hypot(error.x(), error.y()))
                    break;
                damping *= 0.5;
            }
            q -= step * damping;
        }
        if (!converged && std::hypot((sample(q) - p).x(), (sample(q) - p).y()) >= tolerance)
            return std::nullopt;
    }
    q = to_preview_.map(q);
    if (!std::isfinite(q.x()) || !std::isfinite(q.y()))
        return std::nullopt;
    return q;
}
