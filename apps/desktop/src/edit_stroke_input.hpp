#pragma once

#include <QPointF>
#include <QVariantList>
#include <QVector>

#include <optional>

// Validates the narrow QML-to-controller transport used by direct brush
// gestures. Presentation owns pointer sampling; feature controllers own the
// resulting mask or retouch semantics.
namespace EditStrokeInput {

struct NormalizedSample {
    QPointF position;
    double pressure = 1.0;

    bool operator==(const NormalizedSample&) const = default;
};

[[nodiscard]] std::optional<QVector<QPointF>>
decodeNormalizedPoints(const QVariantList& values, qsizetype maximum_points);

[[nodiscard]] std::optional<QVector<NormalizedSample>>
decodeNormalizedSamples(const QVariantList& values, qsizetype maximum_points);

} // namespace EditStrokeInput
