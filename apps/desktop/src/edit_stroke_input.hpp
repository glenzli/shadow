#pragma once

#include <QPointF>
#include <QVariantList>
#include <QVector>

#include <optional>
#include <span>

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

// Returns the same bounds midpoint used to position a rendered stroke's
// source coverage. Source-pick UI must anchor offsets here rather than at the
// first pointer sample, which can sit on one edge of a looped brush gesture.
[[nodiscard]] std::optional<QPointF>
normalizedBoundsCenter(std::span<const QPointF> points) noexcept;

} // namespace EditStrokeInput
