#include "backend/export_settings_codec.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

constexpr auto output_recipe_schema = "shadow-output-recipe-20260830.1";
constexpr auto previous_output_recipe_schema = "shadow-output-recipe-20260809.1";

} // namespace

BackendExportOptions ExportSettingsCodec::fromVariantMap(const QVariantMap& values) {
    BackendExportOptions options;
    options.format = values.value(QStringLiteral("format"), options.format).toString().toLower();
    options.max_edge = static_cast<std::uint32_t>(std::clamp(
        values.value(QStringLiteral("maxEdge"), static_cast<int>(options.max_edge)).toInt(),
        0,
        16'384
    ));
    options.jpeg_quality = static_cast<std::uint8_t>(std::clamp(
        values.value(QStringLiteral("quality"), static_cast<int>(options.jpeg_quality)).toInt(),
        1,
        100
    ));
    options.tiff_bit_depth = static_cast<std::uint8_t>(
        values.value(QStringLiteral("bitDepth"), static_cast<int>(options.tiff_bit_depth)).toInt()
    );
    options.color_space =
        values.value(QStringLiteral("colorSpace"), options.color_space).toString().toLower();
    options.resolution_dpi = static_cast<std::uint16_t>(std::clamp(
        values.value(QStringLiteral("resolutionDpi"), static_cast<int>(options.resolution_dpi))
            .toInt(),
        1,
        2'400
    ));
    options.metadata_policy =
        values.value(QStringLiteral("metadataPolicy"), options.metadata_policy)
            .toString()
            .toLower();
    options.creator = values.value(QStringLiteral("creator")).toString().trimmed();
    options.copyright_notice = values.value(QStringLiteral("copyrightNotice")).toString().trimmed();
    options.filename_suffix = values.value(QStringLiteral("filenameSuffix")).toString();
    options.watermark_path = values.value(QStringLiteral("watermarkPath")).toString();
    if (options.watermark_path.startsWith(QStringLiteral("file:"))) {
        options.watermark_path = QUrl(options.watermark_path).toLocalFile();
    }
    options.watermark_opacity = std::clamp(
        values.value(QStringLiteral("watermarkOpacity"), options.watermark_opacity).toDouble(),
        0.0,
        1.0
    );
    options.watermark_scale = std::clamp(
        values.value(QStringLiteral("watermarkScale"), options.watermark_scale).toDouble(),
        0.01,
        1.0
    );
    options.watermark_inset = std::clamp(
        values.value(QStringLiteral("watermarkInset"), options.watermark_inset).toDouble(),
        0.0,
        0.25
    );
    options.watermark_anchor =
        values.value(QStringLiteral("watermarkAnchor"), options.watermark_anchor).toString();
    if (options.format == QStringLiteral("dng")) {
        // Product RAW DNG is source-stage interchange, not a rendered output.
        // Canonicalize every raster-only option so its durable snapshot cannot
        // imply that edits, resizing, metadata, or watermarking were applied.
        const BackendExportOptions defaults;
        options.max_edge = 0;
        options.jpeg_quality = defaults.jpeg_quality;
        options.tiff_bit_depth = defaults.tiff_bit_depth;
        options.color_space = defaults.color_space;
        options.resolution_dpi = defaults.resolution_dpi;
        options.metadata_policy = QStringLiteral("none");
        options.creator.clear();
        options.copyright_notice.clear();
        options.watermark_path.clear();
        options.watermark_opacity = defaults.watermark_opacity;
        options.watermark_scale = defaults.watermark_scale;
        options.watermark_inset = defaults.watermark_inset;
        options.watermark_anchor = defaults.watermark_anchor;
    }
    validate(options);
    return options;
}

BackendExportOptions ExportSettingsCodec::fromDurableJson(const QString& settings_json) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(settings_json.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::invalid_argument(
            std::string("durable export settings must be one JSON object: ")
            + parse_error.errorString().toStdString()
        );
    }
    const QJsonObject object = document.object();
    const QJsonValue schema = object.value(QStringLiteral("schema"));
    const bool legacy_schema = schema.isDouble() && schema.toInt() == 1;
    const bool dated_schema =
        schema.isString()
        && (schema.toString() == QString::fromLatin1(output_recipe_schema)
            || schema.toString() == QString::fromLatin1(previous_output_recipe_schema));
    if (!legacy_schema && !dated_schema) {
        throw std::invalid_argument("unsupported durable output recipe schema");
    }
    return fromVariantMap(object.toVariantMap());
}

QString ExportSettingsCodec::toDurableJson(const BackendExportOptions& options) {
    validate(options);
    const QVariantMap values{
        {QStringLiteral("schema"), QString::fromLatin1(output_recipe_schema)},
        {QStringLiteral("format"), options.format},
        {QStringLiteral("maxEdge"), static_cast<int>(options.max_edge)},
        {QStringLiteral("quality"), static_cast<int>(options.jpeg_quality)},
        {QStringLiteral("bitDepth"), static_cast<int>(options.tiff_bit_depth)},
        {QStringLiteral("colorSpace"), options.color_space},
        {QStringLiteral("resolutionDpi"), static_cast<int>(options.resolution_dpi)},
        {QStringLiteral("metadataPolicy"), options.metadata_policy},
        {QStringLiteral("creator"), options.creator},
        {QStringLiteral("copyrightNotice"), options.copyright_notice},
        {QStringLiteral("watermarkPath"), options.watermark_path},
        {QStringLiteral("watermarkOpacity"), options.watermark_opacity},
        {QStringLiteral("watermarkScale"), options.watermark_scale},
        {QStringLiteral("watermarkInset"), options.watermark_inset},
        {QStringLiteral("watermarkAnchor"), options.watermark_anchor},
    };
    return QString::fromUtf8(QJsonDocument::fromVariant(values).toJson(QJsonDocument::Compact));
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
        {QStringLiteral("format"), values.value(QStringLiteral("format"), defaults.format)},
        {QStringLiteral("maxEdge"),
         values.value(QStringLiteral("maxEdge"), static_cast<int>(defaults.max_edge))},
        {QStringLiteral("quality"),
         values.value(QStringLiteral("quality"), static_cast<int>(defaults.jpeg_quality))},
        {QStringLiteral("bitDepth"),
         values.value(QStringLiteral("bitDepth"), static_cast<int>(defaults.tiff_bit_depth))},
        {QStringLiteral("colorSpace"),
         values.value(QStringLiteral("colorSpace"), defaults.color_space)},
        {QStringLiteral("resolutionDpi"),
         values.value(QStringLiteral("resolutionDpi"), static_cast<int>(defaults.resolution_dpi))},
        {QStringLiteral("metadataPolicy"),
         values.value(QStringLiteral("metadataPolicy"), defaults.metadata_policy)},
        {QStringLiteral("creator"), values.value(QStringLiteral("creator"))},
        {QStringLiteral("copyrightNotice"), values.value(QStringLiteral("copyrightNotice"))},
        {QStringLiteral("filenameSuffix"), values.value(QStringLiteral("filenameSuffix"))},
        {QStringLiteral("watermarkPath"), values.value(QStringLiteral("watermarkPath"))},
        {QStringLiteral("watermarkOpacity"),
         values.value(QStringLiteral("watermarkOpacity"), defaults.watermark_opacity)},
        {QStringLiteral("watermarkScale"),
         values.value(QStringLiteral("watermarkScale"), defaults.watermark_scale)},
        {QStringLiteral("watermarkInset"),
         values.value(QStringLiteral("watermarkInset"), defaults.watermark_inset)},
        {QStringLiteral("watermarkAnchor"),
         values.value(QStringLiteral("watermarkAnchor"), defaults.watermark_anchor)},
    };
}

void ExportSettingsCodec::validate(const BackendExportOptions& options) {
    if (options.format != QStringLiteral("jpeg") && options.format != QStringLiteral("png")
        && options.format != QStringLiteral("tiff") && options.format != QStringLiteral("dng")) {
        throw std::invalid_argument("export format must be jpeg, png, tiff, or dng");
    }
    if (options.jpeg_quality < 1 || options.jpeg_quality > 100) {
        throw std::invalid_argument("JPEG export quality must be in 1..=100");
    }
    if ((options.tiff_bit_depth != 8 && options.tiff_bit_depth != 16)
        || (options.format != QStringLiteral("tiff") && options.tiff_bit_depth != 8)) {
        throw std::invalid_argument(
            "TIFF bit depth must be 8 or 16, and non-TIFF output must use 8"
        );
    }
    if (options.color_space != QStringLiteral("srgb")
        && options.color_space != QStringLiteral("display-p3")) {
        throw std::invalid_argument("output color space must be srgb or display-p3");
    }
    if (options.resolution_dpi < 1 || options.resolution_dpi > 2'400) {
        throw std::invalid_argument("output resolution must be in 1..=2400 DPI");
    }
    if (options.metadata_policy != QStringLiteral("none")
        && options.metadata_policy != QStringLiteral("copyright-only")) {
        throw std::invalid_argument("output metadata policy must be none or copyright-only");
    }
}
