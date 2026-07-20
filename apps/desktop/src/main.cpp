#include "desktop_backend.hpp"
#include "review_controller.hpp"
#include "thumbnail_provider.hpp"

#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

#include <memory>

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Shadow"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("shadow.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("Shadow"));

    const QString application_data =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(application_data);
    const QString catalog_path = QDir(application_data).filePath(QStringLiteral("catalog.sqlite"));
    const QString cache_root = QDir(application_data).filePath(QStringLiteral("cache"));

    std::shared_ptr<DesktopBackend> backend;
    try {
        backend = std::make_shared<DesktopBackend>(catalog_path, cache_root);
    } catch (const std::exception& error) {
        qCritical() << "Cannot start Shadow's local backend:" << error.what();
        return EXIT_FAILURE;
    }
    ReviewController controller(backend);
    QQmlApplicationEngine engine;
    engine.addImageProvider(
        QStringLiteral("shadow"),
        new ThumbnailProvider(backend, controller.reviewModel())
    );
    engine.setInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
    });
    engine.loadFromModule("Shadow.App", "Main");
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }
    const QString initial_folder = qEnvironmentVariable("SHADOW_DESKTOP_SCAN_FOLDER");
    if (!initial_folder.isEmpty()) {
        controller.scanFolder(QUrl::fromLocalFile(initial_folder));
    }
    if (qEnvironmentVariableIsSet("SHADOW_DESKTOP_SMOKE_TEST")) {
        QTimer::singleShot(500, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
