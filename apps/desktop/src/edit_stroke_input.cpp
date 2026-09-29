#include "edit_stroke_input.hpp"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <numeric>

std::optional<QVector<EditStrokeInput::NormalizedSample>>
EditStrokeInput::decodeNormalizedSamples(
    const QVariantList& values,
    const qsizetype maximum_points
) {
    if (maximum_points <= 0 || values.isEmpty() || values.size() > maximum_points) {
        return std::nullopt;
    }

    QVector<NormalizedSample> points;
    points.reserve(values.size());
    for (const QVariant& value : values) {
        const QVariantMap point = value.toMap();
        if (!point.contains(QStringLiteral("x")) || !point.contains(QStringLiteral("y"))) {
            return std::nullopt;
        }
        bool x_ok = false;
        bool y_ok = false;
        bool pressure_ok = true;
        const double x = point.value(QStringLiteral("x")).toDouble(&x_ok);
        const double y = point.value(QStringLiteral("y")).toDouble(&y_ok);
        const double pressure = point.contains(QStringLiteral("pressure"))
                                    ? point.value(QStringLiteral("pressure")).toDouble(&pressure_ok)
                                    : 1.0;
        if (!x_ok || !y_ok || !std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0
            || y < 0.0 || y > 1.0 || !pressure_ok || !std::isfinite(pressure) || pressure < 0.0
            || pressure > 1.0) {
            return std::nullopt;
        }
        const QPointF position{x, y};
        if (!points.isEmpty() && points.constLast().position == position) {
            points.last().pressure = pressure;
        } else {
            points.push_back({.position = position, .pressure = pressure});
        }
    }
    if (points.isEmpty()) {
        return std::nullopt;
    }
    return points;
}

std::optional<QVector<QPointF>> EditStrokeInput::decodeNormalizedPoints(
    const QVariantList& values,
    const qsizetype maximum_points
) {
    const auto samples = decodeNormalizedSamples(values, maximum_points);
    if (!samples.has_value()) {
        return std::nullopt;
    }
    QVector<QPointF> points;
    points.reserve(samples->size());
    for (const auto& sample : *samples) {
        points.push_back(sample.position);
    }
    return points;
}

std::optional<QPointF>
EditStrokeInput::normalizedBoundsCenter(const std::span<const QPointF> points) noexcept {
    if (points.empty()) {
        return std::nullopt;
    }
    double lower_x = 1.0;
    double upper_x = 0.0;
    double lower_y = 1.0;
    double upper_y = 0.0;
    for (const QPointF& point : points) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || point.x() < 0.0
            || point.x() > 1.0 || point.y() < 0.0 || point.y() > 1.0) {
            return std::nullopt;
        }
        lower_x = std::min(lower_x, point.x());
        upper_x = std::max(upper_x, point.x());
        lower_y = std::min(lower_y, point.y());
        upper_y = std::max(upper_y, point.y());
    }
    return QPointF{
        std::midpoint(lower_x, upper_x),
        std::midpoint(lower_y, upper_y),
    };
}
