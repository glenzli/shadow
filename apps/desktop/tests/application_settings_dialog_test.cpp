#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class FakeCacheMaintenance final : public QObject {
    Q_OBJECT

  public:
    Q_INVOKABLE void refreshInventory() {
        ++refresh_count;
    }

    Q_INVOKABLE void planSafeCleanup() {}
    Q_INVOKABLE void runPlannedCleanup() {}

    int refresh_count = 0;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Application settings dialog contract failed: " << message << '\n';
    }
    return condition;
}

void set(QObject& object, const char* const name, const QVariant& value) {
    object.setProperty(name, value);
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);

    QObject ui;
    set(ui, "appearanceMode", QStringLiteral("system"));
    set(ui, "languageMode", QStringLiteral("system"));
    set(ui, "libraryThumbnailScale", 188);
    set(ui, "exifFields", QStringList{QStringLiteral("camera"), QStringLiteral("lens")});

    QObject ai;
    set(ai, "rawDenoiseExecutionAllowed", true);
    set(ai, "subjectMaskExecutionAllowed", true);
    set(ai, "rawDenoiseDefaultAmount", 100);
    set(ai, "modelStoragePath", QStringLiteral("/tmp/models"));
    set(ai, "modelStorageUrl", QUrl::fromLocalFile(QStringLiteral("/tmp/models")));

    QObject cache;
    set(cache, "diskLimitGiB", 20);
    set(cache, "automaticCleanupAllowed", false);
    set(cache, "cacheRootPath", QStringLiteral("/tmp/cache"));
    set(cache, "cacheRootUrl", QUrl::fromLocalFile(QStringLiteral("/tmp/cache")));

    FakeCacheMaintenance maintenance;
    set(maintenance, "busy", false);
    set(maintenance, "statusText", QString{});
    set(maintenance, "errorText", QString{});
    set(maintenance, "inventory", QVariantMap{});
    set(maintenance, "plannedSweep", QVariantMap{});
    set(maintenance, "hasPlan", false);
    set(maintenance, "overConfiguredLimit", false);

    QObject maps;
    set(maps, "secureStorageAvailable", true);
    set(maps, "googleApiKeyStored", false);
    set(maps, "libraryMapProvider", QStringLiteral("none"));
    set(maps, "googlePlacesAllowed", false);
    set(maps, "googleReverseGeocodingAllowed", false);
    set(maps, "mapStyle", QStringLiteral("roadmap"));
    set(maps, "statusCode", QString{});

    QObject editor;
    set(editor, "active", false);
    set(editor, "rawDenoiseNodeMaterialized", false);
    set(editor, "foundationAiDenoiseAvailable", false);
    set(editor, "foundationAiDenoiseStatusText", QString{});

    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.loadFromModule(
        QStringLiteral("Shadow.ApplicationSettingsContract"),
        QStringLiteral("ApplicationSettingsDialog")
    );
    std::unique_ptr<QObject> dialog{component.createWithInitialProperties({
        {QStringLiteral("preferences"), QVariant::fromValue(&ui)},
        {QStringLiteral("aiPreferences"), QVariant::fromValue(&ai)},
        {QStringLiteral("cachePreferences"), QVariant::fromValue(&cache)},
        {QStringLiteral("cacheMaintenanceController"), QVariant::fromValue(&maintenance)},
        {QStringLiteral("mapProviderPreferences"), QVariant::fromValue(&maps)},
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
        {QStringLiteral("hostWidth"), 1200.0},
        {QStringLiteral("hostHeight"), 800.0},
    })};
    if (!dialog) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    if (!require(
            QMetaObject::invokeMethod(
                dialog.get(),
                "present",
                Q_ARG(QVariant, QVariant(QStringLiteral("ai")))
            ),
            "the dialog exposes a section-aware presentation entry"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();

    QObject* const done =
        dialog->findChild<QObject*>(QStringLiteral("applicationSettingsDoneButton"));
    QObject* const raw_permission =
        dialog->findChild<QObject*>(QStringLiteral("rawDenoiseExecutionPermissionSwitch"));
    QObject* const cache_limit = dialog->findChild<QObject*>(QStringLiteral("cacheLimitSlider"));
    QObject* const unlimited_cache =
        dialog->findChild<QObject*>(QStringLiteral("unlimitedCacheCheckBox"));
    QObject* const automatic_cleanup =
        dialog->findChild<QObject*>(QStringLiteral("automaticCacheCleanupSwitch"));
    QObject* const remote_address =
        dialog->findChild<QObject*>(QStringLiteral("remoteLibraryServerAddressField"));
    if (!require(
            dialog->property("selectedIndex").toInt() == 2,
            "AI can be opened directly from the shared settings entry"
        )
        || !require(
            done != nullptr && raw_permission != nullptr && cache_limit != nullptr,
            "the packaged dialog contains navigation, AI, and storage controls"
        )
        || !require(
            unlimited_cache != nullptr && automatic_cleanup != nullptr
                && raw_permission->property("shadowStyled").toBool()
                && unlimited_cache->property("shadowStyled").toBool()
                && automatic_cleanup->property("shadowStyled").toBool(),
            "settings toggles use the packaged Shadow control family"
        )
        || !require(
            remote_address == nullptr,
            "remote Library sources belong to Library Management rather than Settings"
        )
        || !require(
            std::abs(dialog->property("doneButtonRightInset").toDouble() - 16.0) < 0.5,
            "the Done action stays pinned to the right edge of the settings header"
        )) {
        return EXIT_FAILURE;
    }

    return require(
               QMetaObject::invokeMethod(
                   dialog.get(),
                   "selectSection",
                   Q_ARG(QVariant, QVariant(3))
               ) && dialog->property("selectedIndex").toInt() == 3,
               "the settings shell switches to storage without opening another menu"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "application_settings_dialog_test.moc"
