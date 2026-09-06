#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

enum class XmpDevelopTarget {
    ExposureStops,
    ContrastFactor,
    Highlights,
    Shadows,
    Whites,
    Blacks,
    Texture,
    Clarity,
    Dehaze,
    Vibrance,
    SaturationFactor,
};

struct XmpDevelopAdjustment final {
    QString source_name;
    double source_value = 0.0;
    XmpDevelopTarget target = XmpDevelopTarget::ExposureStops;
    double target_value = 0.0;
};

enum class XmpDevelopIgnoredReason {
    AbsoluteWhiteBalance,
    StructuredAdjustment,
    OutsideControlledMapping,
};

struct XmpDevelopIgnoredField final {
    QString source_name;
    XmpDevelopIgnoredReason reason = XmpDevelopIgnoredReason::OutsideControlledMapping;
};

struct XmpDevelopInvalidField final {
    QString source_name;
    QString source_value;
    QString problem;
};

struct XmpDevelopImport final {
    QString process_version;
    bool already_applied = false;
    QVector<XmpDevelopAdjustment> adjustments;
    QVector<XmpDevelopIgnoredField> ignored_fields;
    QVector<XmpDevelopInvalidField> invalid_fields;
    QString document_error;

    [[nodiscard]] bool canApply() const noexcept {
        return document_error.isEmpty() && invalid_fields.isEmpty() && !adjustments.isEmpty();
    }
};

/// Parses only the bounded Camera Raw develop subset that Shadow can map to a
/// new Grade Node. The result deliberately retains ignored and invalid fields
/// so the product can preview the boundary before changing a photo.
[[nodiscard]] XmpDevelopImport parseXmpDevelopImport(const QByteArray& document);
