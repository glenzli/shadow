#include "backend/export_backend.hpp"
#include "backend/export_settings_codec.hpp"
#include "backend/native_path_input.hpp"
#include "desktop_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrl>

#include <concepts>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}
template <typename Backend>
concept HasLegacyOneShotExport = requires(
    Backend& backend,
    const BackendExportOptions& options
) {
    backend.exportPhoto(
        QString{},
        QString{},
        QString{},
        options
    );
};

template <typename Backend>
concept HasPublicDurableFailureOverride = requires(
    Backend& backend,
    const BackendDurableExportItem& item
) {
    backend.failDurableExportItem(
        item,
        std::uint8_t{0},
        QString{},
        QString{},
        true
    );
};

[[nodiscard]] bool throws_with(
    const auto& operation,
    const std::string_view expected
) {
    try {
        operation();
    } catch (const std::exception& error) {
        return std::string_view(error.what()).find(expected)
            != std::string_view::npos;
    }
    return false;
}

} // namespace

static_assert(std::same_as<
    decltype(std::declval<DesktopBackend&>().exportBackend()),
    ExportBackend&
>);
static_assert(!HasLegacyOneShotExport<DesktopBackend>);
static_assert(!HasPublicDurableFailureOverride<DesktopBackend>);

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);

    const BackendExportOptions defaults =
        ExportSettingsCodec::fromVariantMap({});
    if (!require(defaults.format == QStringLiteral("jpeg"), "default format is JPEG")
        || !require(defaults.max_edge == 0U, "default export keeps full size")
        || !require(defaults.jpeg_quality == 90U, "default JPEG quality is 90")
        || !require(defaults.color_space == QStringLiteral("srgb"),
                    "default output color space is sRGB")
        || !require(defaults.resolution_dpi == 300U,
                    "default print resolution is 300 DPI")
        || !require(defaults.metadata_policy == QStringLiteral("none"),
                    "default output strips metadata")
        || !require(defaults.creator.isEmpty(), "default creator is empty")
        || !require(defaults.copyright_notice.isEmpty(),
                    "default copyright notice is empty")
        || !require(defaults.filename_suffix.isEmpty(), "default suffix is empty")
        || !require(defaults.watermark_path.isEmpty(), "default watermark is absent")
        || !require(defaults.watermark_opacity == 0.72, "default watermark opacity is stable")
        || !require(defaults.watermark_scale == 0.18, "default watermark scale is stable")
        || !require(defaults.watermark_inset == 0.02, "default watermark inset is stable")
        || !require(
            defaults.watermark_anchor == QStringLiteral("bottom-right"),
            "default watermark anchor is stable"
        )) {
        return EXIT_FAILURE;
    }

    const QString watermark_path =
        QStringLiteral("/tmp/Shadow watermark.png");
    const BackendExportOptions normalized =
        ExportSettingsCodec::fromVariantMap({
            {QStringLiteral("format"), QStringLiteral("PNG")},
            {QStringLiteral("maxEdge"), 20'000},
            {QStringLiteral("quality"), 0},
            {QStringLiteral("colorSpace"), QStringLiteral("DISPLAY-P3")},
            {QStringLiteral("resolutionDpi"), 9'999},
            {
                QStringLiteral("metadataPolicy"),
                QStringLiteral("COPYRIGHT-ONLY")
            },
            {QStringLiteral("creator"), QStringLiteral("  Photographer  ")},
            {
                QStringLiteral("copyrightNotice"),
                QStringLiteral("  © Shadow  ")
            },
            {QStringLiteral("filenameSuffix"), QStringLiteral("-proof")},
            {
                QStringLiteral("watermarkPath"),
                QUrl::fromLocalFile(watermark_path).toString()
            },
            {QStringLiteral("watermarkOpacity"), 2.0},
            {QStringLiteral("watermarkScale"), 0.0},
            {QStringLiteral("watermarkInset"), 0.8},
            {QStringLiteral("watermarkAnchor"), QStringLiteral("top-left")},
        });
    if (!require(normalized.format == QStringLiteral("png"), "format is normalized")
        || !require(normalized.max_edge == 16'384U, "maximum edge is clamped")
        || !require(normalized.jpeg_quality == 1U, "quality is clamped")
        || !require(normalized.color_space == QStringLiteral("display-p3"),
                    "output color space is normalized")
        || !require(normalized.resolution_dpi == 2'400U,
                    "print resolution is clamped")
        || !require(normalized.metadata_policy
                        == QStringLiteral("copyright-only"),
                    "metadata policy is normalized")
        || !require(normalized.creator == QStringLiteral("Photographer"),
                    "creator is trimmed")
        || !require(normalized.copyright_notice == QStringLiteral("© Shadow"),
                    "copyright notice is trimmed")
        || !require(
            normalized.filename_suffix == QStringLiteral("-proof"),
            "filename suffix remains a destination-planning field"
        )
        || !require(
            normalized.watermark_path == watermark_path,
            "file URL watermark becomes a local path"
        )
        || !require(normalized.watermark_opacity == 1.0, "opacity is clamped")
        || !require(normalized.watermark_scale == 0.01, "scale is clamped")
        || !require(normalized.watermark_inset == 0.25, "inset is clamped")
        || !require(
            normalized.watermark_anchor == QStringLiteral("top-left"),
            "watermark anchor is retained"
        )) {
        return EXIT_FAILURE;
    }

    const QString durable_json =
        ExportSettingsCodec::toDurableJson(normalized);
    const QJsonDocument durable_document =
        QJsonDocument::fromJson(durable_json.toUtf8());
    const QVariantMap durable_values =
        durable_document.object().toVariantMap();
    const BackendExportOptions decoded =
        ExportSettingsCodec::fromDurableJson(durable_json);
    if (!require(durable_document.isObject(), "durable settings are one JSON object")
        || !require(
            durable_values.value(QStringLiteral("schema")).toString()
                == QStringLiteral("shadow-output-recipe-20260809.1"),
            "durable settings use the dated output recipe schema"
        )
        || !require(!durable_values.contains(QStringLiteral("filenameSuffix")),
                    "destination suffix is not frozen into item encoder settings")
        || !require(decoded.format == normalized.format, "format round-trips")
        || !require(decoded.max_edge == normalized.max_edge, "maximum edge round-trips")
        || !require(decoded.jpeg_quality == normalized.jpeg_quality, "quality round-trips")
        || !require(decoded.color_space == normalized.color_space,
                    "output color space round-trips")
        || !require(decoded.resolution_dpi == normalized.resolution_dpi,
                    "print resolution round-trips")
        || !require(decoded.metadata_policy == normalized.metadata_policy,
                    "metadata policy round-trips")
        || !require(decoded.creator == normalized.creator,
                    "creator round-trips")
        || !require(decoded.copyright_notice == normalized.copyright_notice,
                    "copyright notice round-trips")
        || !require(decoded.filename_suffix.isEmpty(), "durable decode has no UI suffix")
        || !require(decoded.watermark_path == normalized.watermark_path,
                    "watermark path round-trips")
        || !require(decoded.watermark_opacity == normalized.watermark_opacity,
                    "watermark opacity round-trips")
        || !require(decoded.watermark_scale == normalized.watermark_scale,
                    "watermark scale round-trips")
        || !require(decoded.watermark_inset == normalized.watermark_inset,
                    "watermark inset round-trips")
        || !require(decoded.watermark_anchor == normalized.watermark_anchor,
                    "watermark anchor round-trips")) {
        return EXIT_FAILURE;
    }

    if (!require(
            throws_with(
                [] {
                    static_cast<void>(
                        ExportSettingsCodec::fromVariantMap({
                            {
                                QStringLiteral("format"),
                                QStringLiteral("gif")
                            },
                        })
                    );
                },
                "export format must be jpeg, png, tiff, or dng"
            ),
            "unsupported formats retain the stable validation error"
        )
        || !require(
            ExportSettingsCodec::fromVariantMap({
                {QStringLiteral("format"), QStringLiteral("tiff")},
            }).format == QStringLiteral("tiff"),
            "TIFF is an accepted photographic output format"
        )
        || !require(
            ExportSettingsCodec::fromVariantMap({
                {QStringLiteral("format"), QStringLiteral("dng")},
                {QStringLiteral("maxEdge"), 2'048},
                {QStringLiteral("quality"), 1},
                {QStringLiteral("colorSpace"), QStringLiteral("display-p3")},
                {QStringLiteral("resolutionDpi"), 72},
                {
                    QStringLiteral("metadataPolicy"),
                    QStringLiteral("copyright-only")
                },
                {QStringLiteral("creator"), QStringLiteral("Photographer")},
                {
                    QStringLiteral("copyrightNotice"),
                    QStringLiteral("Copyright")
                },
                {QStringLiteral("filenameSuffix"), QStringLiteral("-raw")},
                {
                    QStringLiteral("watermarkPath"),
                    QStringLiteral("/tmp/watermark.png")
                },
            }).format == QStringLiteral("dng"),
            "DNG is an accepted source-stage output format"
        )
        || !require(
            ExportSettingsCodec::fromDurableJson(QStringLiteral(
                R"({"schema":1,"format":"jpeg","maxEdge":0,"quality":90})"
            )).color_space == QStringLiteral("srgb"),
            "legacy schema 1 queue items recover with current defaults"
        )
        || !require(
            throws_with(
                [] {
                    static_cast<void>(
                        ExportSettingsCodec::fromDurableJson(QStringLiteral(
                            R"({"schema":"shadow-output-recipe-20990101.1","format":"jpeg"})"
                        ))
                    );
                },
                "unsupported durable output recipe schema"
            ),
            "unknown dated recipe schemas fail before render"
        )
        || !require(
            throws_with(
                [] {
                    static_cast<void>(
                        ExportSettingsCodec::fromDurableJson(
                            QStringLiteral("[]")
                        )
                    );
                },
                "durable export settings must be one JSON object"
            ),
            "non-object durable settings fail before render"
        )) {
        return EXIT_FAILURE;
    }

    const BackendExportOptions dng = ExportSettingsCodec::fromVariantMap({
        {QStringLiteral("format"), QStringLiteral("dng")},
        {QStringLiteral("maxEdge"), 2'048},
        {QStringLiteral("quality"), 1},
        {QStringLiteral("colorSpace"), QStringLiteral("display-p3")},
        {QStringLiteral("resolutionDpi"), 72},
        {
            QStringLiteral("metadataPolicy"),
            QStringLiteral("copyright-only")
        },
        {QStringLiteral("creator"), QStringLiteral("Photographer")},
        {QStringLiteral("copyrightNotice"), QStringLiteral("Copyright")},
        {QStringLiteral("filenameSuffix"), QStringLiteral("-raw")},
        {
            QStringLiteral("watermarkPath"),
            QStringLiteral("/tmp/watermark.png")
        },
    });
    if (!require(dng.max_edge == 0U, "DNG never resizes the source CFA")
        || !require(dng.jpeg_quality == defaults.jpeg_quality,
                    "DNG clears raster quality")
        || !require(dng.color_space == QStringLiteral("srgb"),
                    "DNG clears rendered output colour space")
        || !require(dng.resolution_dpi == defaults.resolution_dpi,
                    "DNG clears print resolution")
        || !require(dng.metadata_policy == QStringLiteral("none"),
                    "DNG clears raster metadata policy")
        || !require(dng.creator.isEmpty(), "DNG clears creator metadata")
        || !require(dng.copyright_notice.isEmpty(),
                    "DNG clears copyright metadata")
        || !require(dng.watermark_path.isEmpty(), "DNG clears watermark")
        || !require(dng.filename_suffix == QStringLiteral("-raw"),
                    "DNG retains destination filename suffix")) {
        return EXIT_FAILURE;
    }

    const QVariantMap preset = ExportSettingsCodec::normalizedPreset(
        QStringLiteral("preset-id"),
        QStringLiteral("Preset"),
        {}
    );
    if (!require(preset.size() == 16, "preset schema retains all sixteen fields")
        || !require(preset.value(QStringLiteral("format")).toString()
                        == QStringLiteral("jpeg"),
                    "preset format default is stable")
        || !require(preset.value(QStringLiteral("quality")).toInt() == 90,
                    "preset quality default is stable")
        || !require(preset.value(QStringLiteral("watermarkOpacity")).toDouble()
                        == 0.72,
                    "preset watermark defaults are stable")
        || !require(preset.value(QStringLiteral("resolutionDpi")).toInt()
                        == 300,
                    "preset print resolution default is stable")
        || !require(preset.value(QStringLiteral("metadataPolicy")).toString()
                        == QStringLiteral("none"),
                    "preset privacy default is stable")) {
        return EXIT_FAILURE;
    }

    QTemporaryDir root;
    if (!require(root.isValid(), "temporary export contract root is available")) {
        return EXIT_FAILURE;
    }
    auto session = shadow::desktop::open_desktop_session(
        native_path_input::path(root.filePath(QStringLiteral("catalog.sqlite"))),
        native_path_input::path(root.filePath(QStringLiteral("cache")))
    );
    ExportBackend backend(*session);
    const BackendDurableExportRecovery recovery =
        backend.recoverDurableExportQueue();
    if (!require(recovery.interrupted_items == 0U, "new queue has no interruption")
        || !require(recovery.requeued_items == 0U, "new queue requeues nothing")
        || !require(recovery.queued_items == 0U, "new queue is empty")
        || !require(!backend.claimNextDurableExportItem().has_value(),
                    "new queue has no claimable item")
        || !require(
            throws_with(
                [&backend, &durable_json] {
                    static_cast<void>(backend.enqueueDurableExportJob(
                        {},
                        durable_json
                    ));
                },
                "select at least one photo to export"
            ),
            "the extracted backend reaches the real durable queue contract"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
