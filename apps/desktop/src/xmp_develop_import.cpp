#include "xmp_develop_import.hpp"

#include <QMap>
#include <QSet>
#include <QXmlStreamReader>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr auto CAMERA_RAW_NAMESPACE = "http://ns.adobe.com/camera-raw-settings/1.0/";

struct Mapping final {
    const char* source_name;
    XmpDevelopTarget target;
    double minimum;
    double maximum;
};

constexpr auto MAPPINGS = std::to_array<Mapping>({
    {"Exposure2012", XmpDevelopTarget::ExposureStops, -16.0, 16.0},
    {"Contrast2012", XmpDevelopTarget::ContrastFactor, -100.0, 100.0},
    {"Highlights2012", XmpDevelopTarget::Highlights, -100.0, 100.0},
    {"Shadows2012", XmpDevelopTarget::Shadows, -100.0, 100.0},
    {"Whites2012", XmpDevelopTarget::Whites, -100.0, 100.0},
    {"Blacks2012", XmpDevelopTarget::Blacks, -100.0, 100.0},
    {"Texture", XmpDevelopTarget::Texture, -100.0, 100.0},
    {"Clarity2012", XmpDevelopTarget::Clarity, -100.0, 100.0},
    {"Dehaze", XmpDevelopTarget::Dehaze, -100.0, 100.0},
    {"Vibrance", XmpDevelopTarget::Vibrance, -100.0, 100.0},
    {"Saturation", XmpDevelopTarget::SaturationFactor, -100.0, 100.0},
});

[[nodiscard]] double map_value(const XmpDevelopTarget target, const double source) {
    switch (target) {
    case XmpDevelopTarget::ExposureStops:
        return source;
    case XmpDevelopTarget::ContrastFactor:
        return qPow(2.0, source / 100.0);
    case XmpDevelopTarget::SaturationFactor:
        return 1.0 + source / 100.0;
    case XmpDevelopTarget::Highlights:
    case XmpDevelopTarget::Shadows:
    case XmpDevelopTarget::Whites:
    case XmpDevelopTarget::Blacks:
    case XmpDevelopTarget::Texture:
    case XmpDevelopTarget::Clarity:
    case XmpDevelopTarget::Dehaze:
    case XmpDevelopTarget::Vibrance:
        return source / 100.0;
    }
    return 0.0;
}

[[nodiscard]] XmpDevelopIgnoredReason ignored_reason(const QString& name) {
    if (name == QStringLiteral("WhiteBalance") || name == QStringLiteral("Temperature")
        || name == QStringLiteral("Tint")) {
        return XmpDevelopIgnoredReason::AbsoluteWhiteBalance;
    }
    if (name.startsWith(QStringLiteral("ToneCurve"))
        || name.startsWith(QStringLiteral("Parametric"))
        || name.startsWith(QStringLiteral("HueAdjustment"))
        || name.startsWith(QStringLiteral("SaturationAdjustment"))
        || name.startsWith(QStringLiteral("LuminanceAdjustment"))
        || name.startsWith(QStringLiteral("SplitToning"))
        || name.startsWith(QStringLiteral("ColorGrade"))
        || name.startsWith(QStringLiteral("Mask"))) {
        return XmpDevelopIgnoredReason::StructuredAdjustment;
    }
    return XmpDevelopIgnoredReason::OutsideControlledMapping;
}

void capture_field(
    QMap<QString, QString>& fields,
    QSet<QString>& conflicting_fields,
    const QString& name,
    const QString& raw_value
) {
    const QString value = raw_value.trimmed();
    const auto existing = fields.constFind(name);
    if (existing != fields.cend() && existing.value() != value) {
        conflicting_fields.insert(name);
        return;
    }
    fields.insert(name, value);
}

} // namespace

XmpDevelopImport parseXmpDevelopImport(const QByteArray& document) {
    XmpDevelopImport result;
    if (document.isEmpty()) {
        result.document_error = QStringLiteral("empty document");
        return result;
    }

    QMap<QString, QString> fields;
    QSet<QString> conflicting_fields;
    QXmlStreamReader xml(document);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) {
            continue;
        }
        for (const auto& attribute : xml.attributes()) {
            if (attribute.namespaceUri() == QLatin1String(CAMERA_RAW_NAMESPACE)) {
                capture_field(
                    fields,
                    conflicting_fields,
                    attribute.name().toString(),
                    attribute.value().toString()
                );
            }
        }
        if (xml.namespaceUri() == QLatin1String(CAMERA_RAW_NAMESPACE)) {
            const QString name = xml.name().toString();
            const QString value = xml.readElementText(QXmlStreamReader::SkipChildElements);
            capture_field(fields, conflicting_fields, name, value);
        }
    }
    if (xml.hasError()) {
        result.document_error = xml.errorString();
        return result;
    }

    result.process_version = fields.take(QStringLiteral("ProcessVersion"));
    fields.remove(QStringLiteral("HasSettings"));
    fields.remove(QStringLiteral("AlreadyApplied"));

    for (const Mapping& mapping : MAPPINGS) {
        const QString name = QString::fromLatin1(mapping.source_name);
        const auto field = fields.find(name);
        if (field == fields.end()) {
            continue;
        }
        const QString raw_value = field.value();
        fields.erase(field);
        if (conflicting_fields.contains(name)) {
            result.invalid_fields.push_back({
                .source_name = name,
                .source_value = raw_value,
                .problem = QStringLiteral("conflicting values"),
            });
            continue;
        }
        bool parsed = false;
        const double source_value = raw_value.toDouble(&parsed);
        if (!parsed || !std::isfinite(source_value)) {
            result.invalid_fields.push_back({
                .source_name = name,
                .source_value = raw_value,
                .problem = QStringLiteral("not a finite number"),
            });
            continue;
        }
        if (source_value < mapping.minimum || source_value > mapping.maximum) {
            result.invalid_fields.push_back({
                .source_name = name,
                .source_value = raw_value,
                .problem = QStringLiteral("outside supported range"),
            });
            continue;
        }
        result.adjustments.push_back({
            .source_name = name,
            .source_value = source_value,
            .target = mapping.target,
            .target_value = map_value(mapping.target, source_value),
        });
    }

    for (auto field = fields.cbegin(); field != fields.cend(); ++field) {
        if (conflicting_fields.contains(field.key())) {
            result.invalid_fields.push_back({
                .source_name = field.key(),
                .source_value = field.value(),
                .problem = QStringLiteral("conflicting values"),
            });
            continue;
        }
        result.ignored_fields.push_back({
            .source_name = field.key(),
            .reason = ignored_reason(field.key()),
        });
    }
    std::ranges::sort(result.ignored_fields, {}, &XmpDevelopIgnoredField::source_name);
    std::ranges::sort(result.invalid_fields, {}, &XmpDevelopInvalidField::source_name);
    return result;
}
