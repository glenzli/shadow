#include "ui_preferences.hpp"

#include <QGuiApplication>
#include <QTemporaryDir>

#include <cstdlib>

namespace {

[[nodiscard]] bool expect(const bool condition) {
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qunsetenv("SHADOW_DESKTOP_APPEARANCE_MODE");
    qunsetenv("SHADOW_DESKTOP_LANGUAGE_MODE");
    QGuiApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) {
        return EXIT_FAILURE;
    }
    const QString settings_path = root.filePath(QStringLiteral("preferences.ini"));

    {
        UiPreferences preferences(application, settings_path);
        preferences.setAppearanceMode(QStringLiteral("dark"));
        preferences.setLanguageMode(QStringLiteral("en"));
        preferences.setLibraryThumbnailScale(284);
        preferences.setExifFieldVisible(QStringLiteral("cfa"), true);
        preferences.setExifFieldVisible(QStringLiteral("lens"), false);
        if (!expect(preferences.appearanceMode() == QStringLiteral("dark"))
            || !expect(preferences.effectiveAppearance() == QStringLiteral("dark"))
            || !expect(preferences.dark())
            || !expect(preferences.languageMode() == QStringLiteral("en"))
            || !expect(preferences.effectiveLanguage() == QStringLiteral("en"))
            || !expect(preferences.exifFieldVisible(QStringLiteral("cfa")))
            || !expect(!preferences.exifFieldVisible(QStringLiteral("lens")))
            || !expect(preferences.libraryThumbnailScale() == 284)) {
            return EXIT_FAILURE;
        }
    }

    {
        UiPreferences reopened(application, settings_path);
        if (!expect(reopened.appearanceMode() == QStringLiteral("dark"))
            || !expect(reopened.languageMode() == QStringLiteral("en"))
            || !expect(reopened.exifFieldVisible(QStringLiteral("cfa")))
            || !expect(!reopened.exifFieldVisible(QStringLiteral("lens")))
            || !expect(reopened.libraryThumbnailScale() == 284)) {
            return EXIT_FAILURE;
        }
        reopened.setAppearanceMode(QStringLiteral("unsupported"));
        reopened.setLanguageMode(QStringLiteral("unsupported"));
        if (!expect(reopened.appearanceMode() == QStringLiteral("system"))
            || !expect(reopened.languageMode() == QStringLiteral("system"))) {
            return EXIT_FAILURE;
        }
        reopened.setAppearanceMode(QStringLiteral("dark"));
        reopened.setLanguageMode(QStringLiteral("en"));
        reopened.setLibraryThumbnailScale(1000);
        if (!expect(reopened.libraryThumbnailScale() == 360)) {
            return EXIT_FAILURE;
        }
    }

    qputenv("SHADOW_DESKTOP_APPEARANCE_MODE", QByteArrayLiteral("light"));
    qputenv("SHADOW_DESKTOP_LANGUAGE_MODE", QByteArrayLiteral("en_US"));
    {
        UiPreferences overridden(application, settings_path);
        if (!expect(overridden.appearanceMode() == QStringLiteral("light"))
            || !expect(overridden.effectiveAppearance() == QStringLiteral("light"))
            || !expect(!overridden.dark())
            || !expect(overridden.languageMode() == QStringLiteral("en"))) {
            return EXIT_FAILURE;
        }
        overridden.setAppearanceMode(QStringLiteral("system"));
        overridden.setLanguageMode(QStringLiteral("system"));
    }
    qunsetenv("SHADOW_DESKTOP_APPEARANCE_MODE");
    qunsetenv("SHADOW_DESKTOP_LANGUAGE_MODE");

    UiPreferences final_reopen(application, settings_path);
    return expect(final_reopen.appearanceMode() == QStringLiteral("dark"))
            && expect(final_reopen.languageMode() == QStringLiteral("en"))
            && expect(final_reopen.libraryThumbnailScale() == 360)
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
