#include "desktop_backend.hpp"
#include "edit_controller.hpp"
#include "edit_preview_provider.hpp"
#include "review_controller.hpp"
#include "thumbnail_provider.hpp"

#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSize>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <memory>

namespace {

[[nodiscard]] QString image_provider_request_id(const QString& source) {
    const QUrl url(source);
    QString id = url.path();
    if (id.startsWith(QLatin1Char('/'))) {
        id.remove(0, 1);
    }
    const QString query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty()) {
        id += QLatin1Char('?');
        id += query;
    }
    return id;
}

} // namespace

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
    auto* const thumbnail_provider = new ThumbnailProvider(
        backend,
        controller.reviewModel()
    );
    engine.addImageProvider(
        QStringLiteral("shadow"),
        thumbnail_provider
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
    const bool record_first_comparison = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_RECORD_FIRST_COMPARISON"
    );
    const bool forget_recorded_comparison = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_FORGET_RECORDED_COMPARISON"
    );
    const bool set_first_decision = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_SET_FIRST_DECISION"
    );
    const bool undo_first_decision = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_UNDO_FIRST_DECISION"
    );
    const bool request_before = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_REQUEST_BEFORE"
    );
    if (open_first_edit && !record_first_comparison && !set_first_decision) {
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
    if (record_first_comparison) {
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            [&controller, thumbnail_provider]() {
                auto* model = controller.reviewModel();
                if (model->rowCount() < 2 || controller.comparisonBusy()
                    || controller.sessionEvidenceCount() > 0) {
                    return;
                }
                const QModelIndex left = model->index(0, 0);
                const QModelIndex right = model->index(1, 0);
                const QVariantMap presentation = controller.prepareComparison(
                    model->data(left, ReviewModel::VisualHandleRole).toString(),
                    model->data(right, ReviewModel::VisualHandleRole).toString()
                );
                const QString presentation_id =
                    presentation.value(QStringLiteral("presentationId")).toString();
                const QString left_ticket =
                    presentation.value(QStringLiteral("leftRequestTicket")).toString();
                const QString right_ticket =
                    presentation.value(QStringLiteral("rightRequestTicket")).toString();
                if (presentation_id.isEmpty() || left_ticket.isEmpty()
                    || right_ticket.isEmpty()) {
                    return;
                }
                const QSize requested_size(1'280, 960);
                QSize left_size;
                QSize right_size;
                const QImage left_image = thumbnail_provider->requestImage(
                    image_provider_request_id(
                        presentation.value(QStringLiteral("leftSource")).toString()
                    ),
                    &left_size,
                    requested_size
                );
                const QImage right_image = thumbnail_provider->requestImage(
                    image_provider_request_id(
                        presentation.value(QStringLiteral("rightSource")).toString()
                    ),
                    &right_size,
                    requested_size
                );
                if (left_image.isNull() || right_image.isNull()
                    || !controller.confirmComparisonReady(
                        presentation_id,
                        left_ticket,
                        right_ticket
                    )) {
                    return;
                }
                controller.recordComparison(presentation_id, 0);
            }
        );
    }
    if (set_first_decision && !record_first_comparison) {
        auto decision_requested = std::make_shared<bool>(false);
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            [&controller, decision_requested]() {
                auto* model = controller.reviewModel();
                if (*decision_requested || model->rowCount() == 0
                    || controller.decisionBusy()) {
                    return;
                }
                *decision_requested = true;
                const QModelIndex first = model->index(0, 0);
                controller.setPhotoFlag(
                    model->data(first, ReviewModel::PhotoIdRole).toString(),
                    QStringLiteral("picked")
                );
            }
        );
    }
    if (!initial_folder.isEmpty()) {
        controller.scanFolder(QUrl::fromLocalFile(initial_folder));
    }
    if (qEnvironmentVariableIsSet("SHADOW_DESKTOP_SMOKE_TEST")) {
        if (record_first_comparison) {
            auto comparison_succeeded = std::make_shared<bool>(false);
            QObject::connect(
                &controller,
                &ReviewController::comparisonRecorded,
                &application,
                [&application, &controller, forget_recorded_comparison,
                 comparison_succeeded]() {
                    if (forget_recorded_comparison) {
                        controller.undoLastComparison();
                    } else {
                        *comparison_succeeded = true;
                        QTimer::singleShot(50, &application, &QCoreApplication::quit);
                    }
                }
            );
            QObject::connect(
                &controller,
                &ReviewController::comparisonForgotten,
                &application,
                [&application, comparison_succeeded]() {
                    *comparison_succeeded = true;
                    QTimer::singleShot(50, &application, &QCoreApplication::quit);
                }
            );
            QTimer::singleShot(
                30'000,
                &application,
                [&application, comparison_succeeded]() {
                    application.exit(*comparison_succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
                }
            );
        } else if (set_first_decision) {
            auto decision_succeeded = std::make_shared<bool>(false);
            QObject::connect(
                &controller,
                &ReviewController::decisionChanged,
                &application,
                [&application, &controller, undo_first_decision, decision_succeeded](
                    const QString&,
                    const qulonglong,
                    const QString& flag,
                    const int
                ) {
                    if (flag != QStringLiteral("picked")
                        || !controller.canUndoDecision()) {
                        return;
                    }
                    if (undo_first_decision) {
                        QTimer::singleShot(
                            0,
                            &controller,
                            &ReviewController::undoLastDecision
                        );
                    } else {
                        *decision_succeeded = true;
                        QTimer::singleShot(50, &application, &QCoreApplication::quit);
                    }
                }
            );
            QObject::connect(
                &controller,
                &ReviewController::decisionUndone,
                &application,
                [&application, decision_succeeded]() {
                    *decision_succeeded = true;
                    QTimer::singleShot(50, &application, &QCoreApplication::quit);
                }
            );
            QTimer::singleShot(
                30'000,
                &application,
                [&application, decision_succeeded]() {
                    application.exit(*decision_succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
                }
            );
        } else if (open_first_edit) {
            QObject::connect(
                &editor,
                &EditController::previewSourceChanged,
                &application,
                [&application, &editor, request_before]() {
                    if (!editor.previewSource().isEmpty()) {
                        if (request_before) {
                            editor.requestBeforePreview();
                        } else {
                            QTimer::singleShot(50, &application, &QCoreApplication::quit);
                        }
                    }
                }
            );
            if (request_before) {
                QObject::connect(
                    &editor,
                    &EditController::beforePreviewSourceChanged,
                    &application,
                    [&application, &editor]() {
                        if (!editor.beforePreviewSource().isEmpty()) {
                            QTimer::singleShot(50, &application, &QCoreApplication::quit);
                        }
                    }
                );
            }
            QTimer::singleShot(
                30'000,
                &application,
                [&application, &editor, request_before]() {
                    const bool succeeded = !editor.previewSource().isEmpty()
                        && (!request_before || !editor.beforePreviewSource().isEmpty());
                    application.exit(succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
                }
            );
        } else {
            QTimer::singleShot(500, &application, &QCoreApplication::quit);
        }
    }
    return application.exec();
}
