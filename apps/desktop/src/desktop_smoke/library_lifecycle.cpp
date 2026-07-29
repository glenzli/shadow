#include "library_lifecycle.hpp"

#include "../edit_controller.hpp"
#include "../review_controller.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDebug>
#include <QMetaObject>
#include <QModelIndex>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>

#include <functional>
#include <memory>

namespace DesktopSmoke {
namespace {

[[nodiscard]] int reviewGridCount(QQmlApplicationEngine& engine) {
    if (engine.rootObjects().isEmpty()) {
        return 0;
    }
    const auto* const grid =
        engine.rootObjects().front()->findChild<QObject*>(QStringLiteral("reviewJustifiedGrid"));
    return grid == nullptr ? 0 : grid->property("count").toInt();
}

[[nodiscard]] bool openFirstPublishedPhoto(
    QQmlApplicationEngine& engine,
    ReviewController& controller,
    EditController& editor
) {
    if (engine.rootObjects().isEmpty()) {
        return false;
    }
    QObject* const root = engine.rootObjects().front();
    QObject* const workspace = root->findChild<QObject*>(QStringLiteral("reviewWorkspace"));
    QAbstractItemModel* const model = controller.reviewModel();
    if (workspace == nullptr || model == nullptr || model->rowCount() == 0) {
        return false;
    }

    const QModelIndex first = model->index(0, 0);
    const QVariantMap card{
        {QStringLiteral("photoId"), model->data(first, ReviewModel::PhotoIdRole)},
        {
            QStringLiteral("representationId"),
            model->data(first, ReviewModel::RepresentationIdRole),
        },
        {
            QStringLiteral("visualHandle"),
            model->data(first, ReviewModel::VisualHandleRole),
        },
        {
            QStringLiteral("decisionHeadSequence"),
            model->data(first, ReviewModel::DecisionHeadSequenceRole),
        },
        {
            QStringLiteral("decisionFlag"),
            model->data(first, ReviewModel::DecisionFlagRole),
        },
        {
            QStringLiteral("decisionRating"),
            model->data(first, ReviewModel::DecisionRatingRole),
        },
        {QStringLiteral("liked"), model->data(first, ReviewModel::LikedRole)},
        {
            QStringLiteral("colorLabel"),
            model->data(first, ReviewModel::ColorLabelRole),
        },
        {QStringLiteral("title"), model->data(first, ReviewModel::TitleRole)},
        {
            QStringLiteral("sourcePath"),
            model->data(first, ReviewModel::SourcePathRole),
        },
        {
            QStringLiteral("visualRole"),
            model->data(first, ReviewModel::VisualRole),
        },
        {
            QStringLiteral("visualSource"),
            model->data(first, ReviewModel::VisualSourceRole),
        },
        {
            QStringLiteral("visualWidth"),
            model->data(first, ReviewModel::VisualWidthRole),
        },
        {
            QStringLiteral("visualHeight"),
            model->data(first, ReviewModel::VisualHeightRole),
        },
    };
    if (!QMetaObject::invokeMethod(
            workspace,
            "selectPhoto",
            Qt::DirectConnection,
            Q_ARG(QVariant, QVariant::fromValue(card)),
            Q_ARG(QVariant, QVariant::fromValue(0))
        )
        || !workspace->property("canOpenSelectedPhoto").toBool()
        || !QMetaObject::invokeMethod(workspace, "openSelectedPhoto", Qt::DirectConnection)) {
        return false;
    }
    return editor.active() && root->property("workspaceIndex").toInt() == 1;
}

} // namespace

void startCancelScanLifecycle(QApplication& application, ReviewController& controller) {
    auto cancel_requested = std::make_shared<bool>(false);
    auto succeeded = std::make_shared<bool>(false);
    QObject::connect(
        &controller,
        &ReviewController::scanProgressChanged,
        &application,
        [&controller, cancel_requested]() {
            if (*cancel_requested || !controller.scanning()
                || controller.scanProgress().value(QStringLiteral("filesSeen")).toULongLong()
                       == 0) {
                return;
            }
            *cancel_requested = true;
            controller.cancelScan();
        }
    );
    const auto finish_cancel_smoke = [&application, &controller, cancel_requested, succeeded]() {
        if (*succeeded || !*cancel_requested || controller.scanning() || controller.refreshing()
            || controller.statusText().startsWith(QStringLiteral("Final Library refresh failed"))
            || controller.scanProgress().value(QStringLiteral("phase")).toString()
                   != QStringLiteral("cancelled")) {
            return;
        }
        *succeeded = true;
        qInfo() << "Cooperative import cancellation smoke passed";
        QTimer::singleShot(50, &application, &QCoreApplication::quit);
    };
    QObject::connect(
        &controller,
        &ReviewController::itemCountChanged,
        &application,
        finish_cancel_smoke
    );
    QObject::connect(
        &controller,
        &ReviewController::refreshingChanged,
        &application,
        finish_cancel_smoke
    );
    QTimer::singleShot(30'000, &application, [&application, cancel_requested, succeeded]() {
        if (!*succeeded) {
            qCritical() << "Cooperative import cancellation smoke failed"
                        << "requested" << *cancel_requested;
            application.exit(EXIT_FAILURE);
        }
    });
}

void startStreamingScanLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller,
    EditController& editor
) {
    auto early_model_visible = std::make_shared<bool>(false);
    auto early_qml_visible = std::make_shared<bool>(false);
    auto early_edit_attempted = std::make_shared<bool>(false);
    auto early_edit_accepted = std::make_shared<bool>(false);
    auto succeeded = std::make_shared<bool>(false);
    auto evaluate = std::make_shared<std::function<void()>>();
    auto observe_justified_grid = std::make_shared<std::function<void()>>();
    *evaluate = [&application,
                 &controller,
                 &engine,
                 early_model_visible,
                 early_qml_visible,
                 early_edit_attempted,
                 early_edit_accepted,
                 succeeded]() {
        if (*succeeded || controller.scanning() || controller.refreshing()
            || controller.reviewModel()->rowCount() == 0 || reviewGridCount(engine) == 0
            || controller.statusText().startsWith(QStringLiteral("Final Library refresh failed"))
            || controller.scanProgress().value(QStringLiteral("phase")).toString()
                   != QStringLiteral("completed")) {
            return;
        }
        *succeeded = true;
        // A normal multi-file import must publish a page during the
        // scan.  A two-file fixture can finish before the first
        // 150ms progress poll, however, so make that a diagnostic
        // rather than treating a fully usable final Library as a
        // false-negative acceptance failure.
        qInfo() << "Streaming import smoke loaded a usable Library"
                << "first page during scan" << (*early_model_visible && *early_qml_visible)
                << "Precision accepted during scan"
                << (*early_edit_attempted && *early_edit_accepted);
        QTimer::singleShot(50, &application, &QCoreApplication::quit);
    };
    *observe_justified_grid = [&controller, &engine, early_qml_visible, evaluate]() {
        if (controller.scanning() && controller.reviewModel()->rowCount() > 0
            && reviewGridCount(engine) > 0) {
            *early_qml_visible = true;
        }
        (*evaluate)();
    };
    QObject::connect(
        &controller,
        &ReviewController::itemCountChanged,
        &application,
        [&application,
         &controller,
         &editor,
         &engine,
         early_model_visible,
         early_edit_attempted,
         early_edit_accepted,
         observe_justified_grid,
         evaluate]() {
            if (controller.scanning() && controller.reviewModel()->rowCount() > 0) {
                *early_model_visible = true;
                if (!*early_edit_attempted) {
                    *early_edit_attempted = true;
                    *early_edit_accepted = openFirstPublishedPhoto(engine, controller, editor);
                    if (!*early_edit_accepted) {
                        qCritical() << "Streaming import exposed a row but "
                                       "Precision rejected it";
                        application.exit(EXIT_FAILURE);
                        return;
                    }
                }
                QTimer::singleShot(0, &application, [observe_justified_grid]() {
                    (*observe_justified_grid)();
                });
                QTimer::singleShot(50, &application, [observe_justified_grid]() {
                    (*observe_justified_grid)();
                });
            }
            (*evaluate)();
        }
    );
    QObject::connect(&controller, &ReviewController::scanningChanged, &application, [evaluate]() {
        (*evaluate)();
    });
    QObject::connect(&controller, &ReviewController::refreshingChanged, &application, [evaluate]() {
        (*evaluate)();
    });
    QTimer::singleShot(
        120'000,
        &application,
        [&application,
         early_model_visible,
         early_qml_visible,
         early_edit_attempted,
         early_edit_accepted,
         succeeded]() {
            if (!*succeeded) {
                qCritical() << "Streaming import smoke failed"
                            << "early model" << *early_model_visible << "early QML"
                            << *early_qml_visible << "early edit attempted" << *early_edit_attempted
                            << "early edit accepted" << *early_edit_accepted;
                application.exit(EXIT_FAILURE);
            }
        }
    );
}

void startReopenLibraryLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller
) {
    auto succeeded = std::make_shared<bool>(false);
    QObject::connect(
        &controller,
        &ReviewController::itemCountChanged,
        &application,
        [&application, &controller, &engine, succeeded]() {
            QTimer::singleShot(0, &application, [&application, &controller, &engine, succeeded]() {
                if (*succeeded || controller.scanning() || controller.refreshing()
                    || controller.reviewModel()->rowCount() == 0 || reviewGridCount(engine) == 0) {
                    return;
                }
                *succeeded = true;
                qInfo() << "Reopen smoke loaded the persisted Library without scanning";
                QTimer::singleShot(50, &application, &QCoreApplication::quit);
            });
        }
    );
    QTimer::singleShot(30'000, &application, [&application, succeeded]() {
        application.exit(*succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
    });
}

} // namespace DesktopSmoke
