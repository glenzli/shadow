#include "backend/export_settings_codec.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <stdexcept>
#include <string>

BackendExportOptions ExportSettingsCodec::fromVariantMap(
    const QVariantMap& values
) {
    BackendExportOptions options;
    options.format = values.value(QStringLiteral("format"), options.format)
                         .toString()
                         .toLower();
    options.max_edge = static_cast<std::uint32_t>(
        std::clamp(
            values.value(
                QStringLiteral("maxEdge"),
                static_cast<int>(options.max_edge)
            ).toInt(),
            0,
            16'384
        )
    );
    options.jpeg_quality = static_cast<std::uint8_t>(
        std::clamp(
            values.value(
                QStringLiteral("quality"),
                static_cast<int>(options.jpeg_quality)
            ).toInt(),
            1,
            100
        )
    );
    options.filename_suffix =
        values.value(QStringLiteral("filenameSuffix")).toString();
    options.watermark_path =
        values.value(QStringLiteral("watermarkPath")).toString();
    if (options.watermark_path.startsWith(QStringLiteral("file:"))) {
        options.watermark_path = QUrl(options.watermark_path).toLocalFile();
    }
    options.watermark_opacity = std::clamp(
        values.value(
            QStringLiteral("watermarkOpacity"),
            options.watermark_opacity
        ).toDouble(),
        0.0,
        1.0
    );
    options.watermark_scale = std::clamp(
        values.value(
            QStringLiteral("watermarkScale"),
            options.watermark_scale
        ).toDouble(),
        0.01,
        1.0
    );
    options.watermark_inset = std::clamp(
        values.value(
            QStringLiteral("watermarkInset"),
            options.watermark_inset
        ).toDouble(),
        0.0,
        0.25
    );
    options.watermark_anchor = values.value(
        QStringLiteral("watermarkAnchor"),
        options.watermark_anchor
    ).toString();
    validate(options);
    return options;
}

BackendExportOptions ExportSettingsCodec::fromDurableJson(
    const QString& settings_json
) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(
        settings_json.toUtf8(),
        &parse_error
    );
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::invalid_argument(
            std::string("durable export settings must be one JSON object: ")
            + parse_error.errorString().toStdString()
        );
    }
    return fromVariantMap(document.object().toVariantMap());
}

QString ExportSettingsCodec::toDurableJson(
    const BackendExportOptions& options
) {
    validate(options);
    const QVariantMap values{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("format"), options.format},
        {QStringLiteral("maxEdge"), static_cast<int>(options.max_edge)},
        {QStringLiteral("quality"), static_cast<int>(options.jpeg_quality)},
        {QStringLiteral("watermarkPath"), options.watermark_path},
        {QStringLiteral("watermarkOpacity"), options.watermark_opacity},
        {QStringLiteral("watermarkScale"), options.watermark_scale},
        {QStringLiteral("watermarkInset"), options.watermark_inset},
        {QStringLiteral("watermarkAnchor"), options.watermark_anchor},
    };
    return QString::fromUtf8(
        QJsonDocument::fromVariant(values).toJson(QJsonDocument::Compact)
    );
}

QVariantMap ExportSettingsCodec::normalizedPreset(
    const QString& id,
    const QString& name,
    const QVariantMap& values
) {
    const BackendExportOptions defaults;
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {
            QStringLiteral("format"),
            values.value(QStringLiteral("format"), defaults.format)
        },
        {
            QStringLiteral("maxEdge"),
            values.value(
                QStringLiteral("maxEdge"),
                static_cast<int>(defaults.max_edge)
            )
        },
        {
            QStringLiteral("quality"),
            values.value(
                QStringLiteral("quality"),
                static_cast<int>(defaults.jpeg_quality)
            )
        },
        {
            QStringLiteral("filenameSuffix"),
            values.value(QStringLiteral("filenameSuffix"))
        },
        {
            QStringLiteral("watermarkPath"),
            values.value(QStringLiteral("watermarkPath"))
        },
        {
            QStringLiteral("watermarkOpacity"),
            values.value(
                QStringLiteral("watermarkOpacity"),
                defaults.watermark_opacity
            )
        },
        {
            QStringLiteral("watermarkScale"),
            values.value(
                QStringLiteral("watermarkScale"),
                defaults.watermark_scale
            )
        },
        {
            QStringLiteral("watermarkInset"),
            values.value(
                QStringLiteral("watermarkInset"),
                defaults.watermark_inset
            )
        },
        {
            QStringLiteral("watermarkAnchor"),
            values.value(
                QStringLiteral("watermarkAnchor"),
                defaults.watermark_anchor
            )
        },
    };
}

void ExportSettingsCodec::validate(const BackendExportOptions& options) {
    if (options.format != QStringLiteral("jpeg")
        && options.format != QStringLiteral("png")) {
        throw std::invalid_argument("export format must be jpeg or png");
    }
    if (options.jpeg_quality < 1 || options.jpeg_quality > 100) {
        throw std::invalid_argument("JPEG export quality must be in 1..=100");
    }
}
