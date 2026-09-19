#include "library_lifecycle.hpp"

#include "../edit_controller.hpp"
#include "../justified_review_layout_model.hpp"
#include "../review_controller.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
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
    auto first_page_ms = std::make_shared<qint64>(-1);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    *evaluate = [&application,
                 &controller,
                 &engine,
                 early_model_visible,
                 early_qml_visible,
                 early_edit_attempted,
                 early_edit_accepted,
                 first_page_ms,
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
                << "first page ms" << *first_page_ms << "Precision accepted during scan"
                << (*early_edit_attempted && *early_edit_accepted);
        QTimer::singleShot(50, &application, &QCoreApplication::quit);
    };
    *observe_justified_grid =
        [&controller, &engine, early_qml_visible, first_page_ms, clock, evaluate]() {
            if (controller.scanning() && controller.reviewModel()->rowCount() > 0
                && reviewGridCount(engine) > 0) {
                *early_qml_visible = true;
                if (*first_page_ms < 0) {
                    *first_page_ms = clock->elapsed();
                }
            }
            (*evaluate)();
        };
    const auto observe_published_rows = [&application,
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
    };
    QObject::connect(
        &controller,
        &ReviewController::itemCountChanged,
        &application,
        observe_published_rows
    );
    // Counts are a separate asynchronous query and are not refreshed for a
    // streaming prefix. Observe the actual presentation rows; otherwise a
    // long, responsive import is falsely reported as having no early page.
    const auto defer_observation = [&application, observe_published_rows]() {
        QTimer::singleShot(0, &application, observe_published_rows);
    };
    QObject::connect(
        controller.reviewModel(),
        &QAbstractItemModel::modelReset,
        &application,
        defer_observation
    );
    QObject::connect(
        controller.reviewModel(),
        &QAbstractItemModel::rowsInserted,
        &application,
        [defer_observation](const QModelIndex&, int, int) { defer_observation(); }
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

void startRemoteLibraryLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller
) {
    auto* poll = new QTimer(&application);
    poll->setInterval(100);
    QObject* const workspace =
        engine.rootObjects().front()->findChild<QObject*>(QStringLiteral("reviewWorkspace"));
    QObject* const count_label =
        engine.rootObjects().front()->findChild<QObject*>(QStringLiteral("allPhotosCount"));
    QObject* const grid =
        engine.rootObjects().front()->findChild<QObject*>(QStringLiteral("reviewJustifiedGrid"));
    auto phase = std::make_shared<int>(0);
    QObject::connect(
        poll,
        &QTimer::timeout,
        &application,
        [&, poll, workspace, count_label, grid, phase]() {
            if (workspace == nullptr || count_label == nullptr || grid == nullptr) {
                qCritical() << "Remote Library smoke requires the packaged collection controls";
                application.exit(EXIT_FAILURE);
                return;
            }
            if (controller.remoteLibrarySyncing() || controller.refreshing()
                || !controller.librarySystemCollectionCounts()
                        .value(QStringLiteral("available"))
                        .toBool()
                || controller.remoteLibraryPhotoCount() == 0) {
                return;
            }
            if (*phase == 0 || *phase == 2) {
                if (!QMetaObject::invokeMethod(
                        workspace,
                        "applySystemCollection",
                        Qt::DirectConnection,
                        Q_ARG(QVariant, QVariant(QStringLiteral("all")))
                    )) {
                    application.exit(EXIT_FAILURE);
                    return;
                }
                ++*phase;
                return;
            }
            const auto* model = controller.model();
            int remote_rows = 0;
            for (int row = 0; row < model->rowCount(); ++row) {
                remote_rows +=
                    model->data(model->index(row, 0), ReviewModel::IsRemoteRole).toBool() ? 1 : 0;
            }
            const auto all = controller.librarySystemCollectionCounts()
                                 .value(QStringLiteral("all"))
                                 .toULongLong();
            const auto* layout = qobject_cast<JustifiedReviewLayoutModel*>(
                grid->property("model").value<QObject*>()
            );
            qsizetype layout_photos = 0;
            int layout_remote = 0;
            if (layout != nullptr) {
                for (int row = 0; row < layout->rowCount(); ++row) {
                    const auto items =
                        layout->data(layout->index(row, 0), JustifiedReviewLayoutModel::ItemsRole)
                            .toList();
                    layout_photos += items.size();
                    for (const auto& item : items) {
                        layout_remote +=
                            item.toMap().value(QStringLiteral("isRemote")).toBool() ? 1 : 0;
                    }
                }
            }
            if (remote_rows != controller.remoteLibraryPhotoCount()
                || all != static_cast<qulonglong>(controller.itemCount())
                || layout_photos != model->rowCount() || layout_remote != remote_rows
                || count_label->property("text").toString() != QStringLiteral("%L1").arg(all)) {
                return;
            }
            if (*phase == 1) {
                controller.setFilterCameraKey(QStringLiteral("remote-smoke-no-local-camera"));
                // A local-only scope hides remote rows, but global collection badges
                // must still include them while the asynchronous local query changes.
                if (controller.librarySystemCollectionCounts()
                        .value(QStringLiteral("all"))
                        .toULongLong()
                    != all) {
                    application.exit(EXIT_FAILURE);
                    return;
                }
                ++*phase;
                return;
            }
            qInfo() << "Remote Library smoke: all" << all << "remote" << remote_rows
                    << "gallery photos" << layout_photos
                    << "sidebar count and All Photos restoration verified";
            poll->stop();
            application.quit();
        }
    );
    poll->start();
    QTimer::singleShot(60'000, &application, [&application, &controller]() {
        qCritical() << "Remote Library smoke timed out: remote"
                    << controller.remoteLibraryPhotoCount() << "total" << controller.itemCount()
                    << "status" << controller.remoteLibraryStatusCode();
        application.exit(EXIT_FAILURE);
    });
}

} // namespace DesktopSmoke
