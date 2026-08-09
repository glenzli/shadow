#include "export_watermark_store.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <iostream>

namespace {

int failures = 0;

void check(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "export watermark store contract failed: "
                  << message << '\n';
        ++failures;
    }
}

[[nodiscard]] QVariantMap watermark_by_id(
    const QVariantList& watermarks,
    const QString& id
) {
    for (const QVariant& value : watermarks) {
        const QVariantMap watermark = value.toMap();
        if (watermark.value(QStringLiteral("id")).toString() == id) {
            return watermark;
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir settings_root;
    check(settings_root.isValid(), "temporary settings root is available");
    const QString settings_path =
        settings_root.filePath(QStringLiteral("watermarks.ini"));
    ExportWatermarkStore store(settings_path);
    check(store.watermarks().isEmpty(), "a fresh watermark library is empty");

    const QString id = store.save(
        QStringLiteral("  Studio mark  "),
        {
            {QStringLiteral("watermarkPath"), QStringLiteral("file:///tmp/logo.png")},
            {QStringLiteral("watermarkOpacity"), 2.0},
            {QStringLiteral("watermarkScale"), 0.2},
            {QStringLiteral("watermarkInset"), -1.0},
            {QStringLiteral("watermarkAnchor"), QStringLiteral("invalid")},
        }
    );
    check(!id.isEmpty(), "a valid definition receives an identity");
    const QVariantMap created = watermark_by_id(store.watermarks(), id);
    check(
        created.value(QStringLiteral("name")).toString()
            == QStringLiteral("Studio mark"),
        "watermark names are trimmed"
    );
    check(
        created.value(QStringLiteral("watermarkPath")).toString()
            == QStringLiteral("/tmp/logo.png"),
        "file URLs persist as local paths"
    );
    check(
        created.value(QStringLiteral("watermarkOpacity")).toDouble() == 1.0
            && created.value(QStringLiteral("watermarkInset")).toDouble() == 0.0
            && created.value(QStringLiteral("watermarkAnchor")).toString()
                == QStringLiteral("bottom-right"),
        "rendering values are normalized"
    );

    check(
        store.update(
            id,
            QStringLiteral("Client mark"),
            {
                {QStringLiteral("watermarkPath"), QStringLiteral("/tmp/client.png")},
                {QStringLiteral("watermarkOpacity"), 0.5},
                {QStringLiteral("watermarkScale"), 0.12},
                {QStringLiteral("watermarkInset"), 0.04},
                {QStringLiteral("watermarkAnchor"), QStringLiteral("top-left")},
            }
        ) == id,
        "an existing watermark can be edited by identity"
    );
    {
        ExportWatermarkStore reopened(settings_path);
        const QVariantMap persisted = watermark_by_id(reopened.watermarks(), id);
        check(
            persisted.value(QStringLiteral("name")).toString()
                == QStringLiteral("Client mark"),
            "watermark edits survive reopen"
        );
        check(reopened.remove(id), "an existing watermark can be removed");
        check(!reopened.remove(id), "missing watermark removal is a no-op");
    }
    {
        ExportWatermarkStore reopened(settings_path);
        check(reopened.watermarks().isEmpty(), "removal survives reopen");
    }
    check(
        store.save(QStringLiteral("No file"), {}).isEmpty(),
        "definitions without a PNG path are rejected"
    );

    return failures == 0 ? 0 : 1;
}
