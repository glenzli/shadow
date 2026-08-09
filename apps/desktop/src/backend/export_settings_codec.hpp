#pragma once

#include <QString>
#include <QVariantMap>

#include <cstdint>

/// The canonical desktop export settings after UI values have been normalized.
/// `filename_suffix` affects destination planning only and is deliberately not
/// persisted in an immutable durable-export item.
struct BackendExportOptions final {
    QString format = QStringLiteral("jpeg");
    std::uint32_t max_edge = 0;
    std::uint8_t jpeg_quality = 90;
    QString color_space = QStringLiteral("srgb");
    std::uint16_t resolution_dpi = 300;
    QString metadata_policy = QStringLiteral("none");
    QString creator;
    QString copyright_notice;
    QString filename_suffix;
    QString watermark_path;
    double watermark_opacity = 0.72;
    double watermark_scale = 0.18;
    double watermark_inset = 0.02;
    QString watermark_anchor = QStringLiteral("bottom-right");
};

/// Owns the field names, defaults, normalization, validation, and durable JSON
/// representation shared by the QML-facing controller and export executor.
class ExportSettingsCodec final {
public:
    [[nodiscard]] static BackendExportOptions fromVariantMap(
        const QVariantMap& values
    );
    [[nodiscard]] static BackendExportOptions fromDurableJson(
        const QString& settings_json
    );
    [[nodiscard]] static QString toDurableJson(
        const BackendExportOptions& options
    );
    [[nodiscard]] static QVariantMap normalizedPreset(
        const QString& id,
        const QString& name,
        const QVariantMap& values
    );
    static void validate(const BackendExportOptions& options);
};
