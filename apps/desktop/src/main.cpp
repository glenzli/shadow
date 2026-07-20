#include "desktop_backend.hpp"
#include "edit_controller.hpp"
#include "edit_preview_provider.hpp"
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

    QString application_data = qEnvironmentVariable("SHADOW_DESKTOP_DATA_ROOT");
    if (application_data.isEmpty()) {
        application_data =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    }
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
    auto edit_preview_store = std::make_shared<EditPreviewStore>();
    EditController editor(backend, edit_preview_store);
    QQmlApplicationEngine engine;
    engine.addImageProvider(
        QStringLiteral("shadow"),
        new ThumbnailProvider(backend, controller.reviewModel())
    );
    engine.addImageProvider(
        QStringLiteral("shadow-edit"),
        new EditPreviewProvider(edit_preview_store)
    );
    engine.setInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
    });
    engine.loadFromModule("Shadow.App", "Main");
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }
    const QString initial_folder = qEnvironmentVariable("SHADOW_DESKTOP_SCAN_FOLDER");
    const bool open_first_edit = qEnvironmentVariableIsSet("SHADOW_DESKTOP_OPEN_FIRST_EDIT");
    if (open_first_edit) {
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            [&controller, &editor]() {
                auto* model = controller.reviewModel();
                if (editor.active() || model->rowCount() == 0) {
                    return;
                }
                const QModelIndex first = model->index(0, 0);
                editor.openPhoto(
                    model->data(first, ReviewModel::PhotoIdRole).toString(),
                    model->data(first, ReviewModel::RepresentationIdRole).toString(),
                    model->data(first, ReviewModel::SourcePathRole).toString(),
                    model->data(first, ReviewModel::TitleRole).toString()
                );
            }
        );
    }
    if (!initial_folder.isEmpty()) {
        controller.scanFolder(QUrl::fromLocalFile(initial_folder));
    }
    if (qEnvironmentVariableIsSet("SHADOW_DESKTOP_SMOKE_TEST")) {
        if (open_first_edit) {
            QObject::connect(
                &editor,
                &EditController::previewSourceChanged,
                &application,
                [&application, &editor]() {
                    if (!editor.previewSource().isEmpty()) {
                        QTimer::singleShot(50, &application, &QCoreApplication::quit);
                    }
                }
            );
            QTimer::singleShot(30'000, &application, [&application, &editor]() {
                application.exit(editor.previewSource().isEmpty() ? EXIT_FAILURE : EXIT_SUCCESS);
            });
        } else {
            QTimer::singleShot(500, &application, &QCoreApplication::quit);
        }
    }
    return application.exec();
}
