#include "edit_stroke_input.hpp"

#include <QVariantMap>

#include <cmath>

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
