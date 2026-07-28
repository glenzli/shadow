#include "export_preset_store.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTranslator>

#include <iostream>

namespace {

int failures = 0;

void check(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "export preset store contract failed: "
                  << message << '\n';
        ++failures;
    }
}

[[nodiscard]] QVariantMap preset_by_id(
    const QVariantList& presets,
    const QString& id
) {
    for (const QVariant& value : presets) {
        const QVariantMap preset = value.toMap();
        if (preset.value(QStringLiteral("id")).toString() == id) {
            return preset;
        }
    }
    return {};
}

class ExportPresetTranslator final : public QTranslator {
public:
    [[nodiscard]] QString translate(
        const char* context,
        const char* source_text,
        const char*,
        int
    ) const override {
        if (QLatin1StringView(context) == QLatin1StringView("ExportController")) {
            return QStringLiteral("测试·")
                + QString::fromUtf8(source_text);
        }
        return {};
    }
};

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir settings_root;
    check(settings_root.isValid(), "temporary settings root is available");

    const QString settings_path =
        settings_root.filePath(QStringLiteral("presets.ini"));
    ExportPresetStore store(settings_path);
    check(store.presets().size() == 3, "a fresh store exposes three built-ins");
    check(
        preset_by_id(
            store.presets(),
            QStringLiteral("builtin-full-jpeg")
        ).value(QStringLiteral("name")).toString()
            == QStringLiteral("Full-size JPEG"),
        "built-in identity resolves its current localized name"
    );

    const QVariantMap options{
        {QStringLiteral("format"), QStringLiteral("jpeg")},
        {QStringLiteral("maxEdge"), 1440},
        {QStringLiteral("quality"), 84},
    };
    const QString custom_id = store.save(
        QStringLiteral("  Small delivery  "),
        options
    );
    check(!custom_id.isEmpty(), "a named custom preset receives an identity");
    check(store.presets().size() == 4, "custom preset is appended once");
    check(
        preset_by_id(store.presets(), custom_id)
                .value(QStringLiteral("name"))
                .toString()
            == QStringLiteral("Small delivery"),
        "custom preset name is trimmed"
    );

    const QString updated_id = store.save(
        QStringLiteral("small DELIVERY"),
        {
            {QStringLiteral("format"), QStringLiteral("png")},
            {QStringLiteral("maxEdge"), 2048},
            {QStringLiteral("quality"), 100},
        }
    );
    check(updated_id == custom_id, "case-insensitive name updates the preset");
    check(store.presets().size() == 4, "updating does not duplicate the preset");
    check(
        preset_by_id(store.presets(), custom_id)
                .value(QStringLiteral("format"))
                .toString()
            == QStringLiteral("png"),
        "updated options replace the persisted projection"
    );
    check(
        store.save(QStringLiteral("   "), {}).isEmpty(),
        "blank preset names are rejected"
    );

    {
        ExportPresetStore reopened(settings_path);
        check(reopened.presets().size() == 4, "custom preset survives reopen");
        check(
            preset_by_id(reopened.presets(), custom_id)
                    .value(QStringLiteral("maxEdge"))
                    .toInt()
                == 2048,
            "reopened preset retains updated options"
        );
        check(reopened.remove(custom_id), "existing custom preset is removed");
        check(!reopened.remove(custom_id), "missing preset removal is a no-op");
    }
    {
        ExportPresetStore reopened(settings_path);
        check(reopened.presets().size() == 3, "removal survives reopen");
    }

    const QString translated_path =
        settings_root.filePath(QStringLiteral("translated.ini"));
    ExportPresetTranslator translator;
    application.installTranslator(&translator);
    ExportPresetStore translated_store(translated_path);
    check(
        preset_by_id(
            translated_store.presets(),
            QStringLiteral("builtin-web-jpeg")
        ).value(QStringLiteral("name")).toString()
            == QStringLiteral("测试·Web JPEG"),
        "built-in names use the active runtime translator"
    );
    const QString translated_custom_id = translated_store.save(
        QStringLiteral("Client"),
        options
    );
    application.removeTranslator(&translator);
    check(
        translated_store.retranslateBuiltins(),
        "language change updates persisted built-in names"
    );
    check(
        preset_by_id(
            translated_store.presets(),
            QStringLiteral("builtin-web-jpeg")
        ).value(QStringLiteral("name")).toString()
            == QStringLiteral("Web JPEG"),
        "built-in name returns to the current language"
    );
    check(
        preset_by_id(translated_store.presets(), translated_custom_id)
                .value(QStringLiteral("name"))
                .toString()
            == QStringLiteral("Client"),
        "user-defined names are not translated"
    );

    return failures == 0 ? 0 : 1;
}
