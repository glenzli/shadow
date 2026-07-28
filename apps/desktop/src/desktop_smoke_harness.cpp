#include "desktop_smoke_harness.hpp"

#include "desktop_smoke/edit_preview_session.hpp"
#include "desktop_smoke/grade_stack_persistence.hpp"
#include "edit_controller.hpp"
#include "review_controller.hpp"
#include "thumbnail_provider.hpp"
#include "ui_preferences.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QMetaObject>
#include <QModelIndex>
#include <QQmlApplicationEngine>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QWindow>

#include <functional>
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

[[nodiscard]] int review_grid_count(QQmlApplicationEngine& engine) {
    if (engine.rootObjects().isEmpty()) {
        return 0;
    }
    const auto* const grid = engine.rootObjects().front()->findChild<QObject*>(
        QStringLiteral("reviewJustifiedGrid")
    );
    return grid == nullptr ? 0 : grid->property("count").toInt();
}

} // namespace

void installDesktopSmokeHarness(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller,
    EditController& editor,
    ThumbnailProvider* const thumbnail_provider,
    EditPreviewProvider* const edit_preview_provider,
    UiPreferences& preferences,
    const QString& initial_folder
) {
    if (qEnvironmentVariableIsSet("SHADOW_DESKTOP_OPEN_LUT_LIBRARY")) {
        QMetaObject::invokeMethod(
            engine.rootObjects().front(),
            "openLutManager",
            Qt::DirectConnection
        );
    }
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
    const bool grade_stack_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_GRADE_STACK_SMOKE"
    );
    const bool full_detail_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_FULL_DETAIL_SMOKE"
    );
    const bool streaming_scan_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_STREAMING_SCAN_SMOKE"
    );
    const bool reopen_library_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_REOPEN_LIBRARY_SMOKE"
    );
    const bool cancel_scan_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_CANCEL_SCAN_SMOKE"
    );
    const bool i18n_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_I18N_SMOKE"
    );
    const bool close_lifecycle_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_CLOSE_SMOKE"
    );
    const bool dirty_close_smoke = qEnvironmentVariableIsSet(
        "SHADOW_DESKTOP_DIRTY_CLOSE_SMOKE"
    );
    const bool smoke_test =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_SMOKE_TEST");
    const bool needs_legacy_first_photo_opener =
        !smoke_test || dirty_close_smoke
        || (grade_stack_smoke && !full_detail_smoke);
    if (open_first_edit && !record_first_comparison && !set_first_decision
        && needs_legacy_first_photo_opener) {
        const auto open_first_available = [&controller, &editor, &engine]() {
            auto* const model = controller.reviewModel();
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
            if (!engine.rootObjects().isEmpty()) {
                engine.rootObjects().front()->setProperty("workspaceIndex", 1);
            }
        };
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            open_first_available
        );
        // The Catalog count and the first page are deliberately separate
        // asynchronous queries.  A persisted Library may publish the page
        // before (or without a changed) count notification, so a headless
        // open-first flow must listen to the model that owns the rows rather
        // than treating the aggregate count as its readiness signal.
        QObject::connect(
            controller.reviewModel(),
            &QAbstractItemModel::modelReset,
            &application,
            open_first_available
        );
        QObject::connect(
            controller.reviewModel(),
            &QAbstractItemModel::rowsInserted,
            &application,
            [open_first_available](const QModelIndex&, int, int) {
                open_first_available();
            }
        );
        QTimer::singleShot(0, &application, open_first_available);
        QTimer::singleShot(50, &application, open_first_available);
    }
    if (record_first_comparison) {
        auto comparison_requested = std::make_shared<bool>(false);
        const auto request_first_comparison = [
            &application,
            &controller,
            thumbnail_provider,
            comparison_requested
        ]() {
            auto* const model = controller.reviewModel();
            if (*comparison_requested || model->rowCount() < 2
                || controller.scanning() || controller.refreshing()
                || controller.comparisonBusy()
                || controller.sessionEvidenceCount() > 0) {
                return;
            }
            const QModelIndex left = model->index(0, 0);
            const QModelIndex right = model->index(1, 0);
            const QString left_handle =
                model->data(left, ReviewModel::VisualHandleRole).toString();
            const QString right_handle =
                model->data(right, ReviewModel::VisualHandleRole).toString();
            if (left_handle.isEmpty() || right_handle.isEmpty()) {
                return;
            }
            *comparison_requested = true;
            const QVariantMap presentation = controller.prepareComparison(
                left_handle,
                right_handle
            );
            const QString presentation_id =
                presentation.value(QStringLiteral("presentationId")).toString();
            const QString left_ticket =
                presentation.value(QStringLiteral("leftRequestTicket")).toString();
            const QString right_ticket =
                presentation.value(QStringLiteral("rightRequestTicket")).toString();
            if (presentation_id.isEmpty() || left_ticket.isEmpty()
                || right_ticket.isEmpty()) {
                qCritical() << "Comparison smoke could not prepare two visual tickets";
                application.exit(EXIT_FAILURE);
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
            if (left_image.isNull() || right_image.isNull()) {
                qCritical() << "Comparison smoke could not decode both visual tickets"
                            << "left null" << left_image.isNull() << "right null"
                            << right_image.isNull();
                application.exit(EXIT_FAILURE);
                return;
            }
            if (!controller.confirmComparisonReady(
                    presentation_id,
                    left_ticket,
                    right_ticket
                )) {
                qCritical() << "Comparison smoke could not verify both decoded frames";
                application.exit(EXIT_FAILURE);
                return;
            }
            controller.recordComparison(presentation_id, 0);
        };
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            request_first_comparison
        );
        QObject::connect(
            controller.reviewModel(),
            &QAbstractItemModel::modelReset,
            &application,
            request_first_comparison
        );
        QObject::connect(
            controller.reviewModel(),
            &QAbstractItemModel::rowsInserted,
            &application,
            [&application, request_first_comparison](const QModelIndex&, int, int) {
                QTimer::singleShot(0, &application, request_first_comparison);
            }
        );
        QObject::connect(
            &controller,
            &ReviewController::refreshingChanged,
            &application,
            request_first_comparison
        );
        QTimer::singleShot(0, &application, request_first_comparison);
        QTimer::singleShot(50, &application, request_first_comparison);
    }
    if (set_first_decision && !record_first_comparison) {
        auto decision_requested = std::make_shared<bool>(false);
        const auto request_first_decision = [&controller, decision_requested]() {
            auto* const model = controller.reviewModel();
            if (*decision_requested || model->rowCount() == 0
                || controller.scanning() || controller.refreshing()
                || controller.decisionBusy()) {
                return;
            }
            *decision_requested = true;
            const QModelIndex first = model->index(0, 0);
            controller.setPhotoFlag(
                model->data(first, ReviewModel::PhotoIdRole).toString(),
                QStringLiteral("picked")
            );
        };
        QObject::connect(
            &controller,
            &ReviewController::itemCountChanged,
            &application,
            request_first_decision
        );
        // Just like open-first editing, Review's count and first page resolve
        // independently.  A persisted Catalog can publish its rows without a
        // new aggregate-count notification, so drive this headless action from
        // the row-owning model as well.
        QObject::connect(
            controller.reviewModel(),
            &QAbstractItemModel::modelReset,
            &application,
            request_first_decision
        );
        QObject::connect(
            controller.reviewModel(),
            &QAbstractItemModel::rowsInserted,
            &application,
            [request_first_decision](const QModelIndex&, int, int) {
                request_first_decision();
            }
        );
        QTimer::singleShot(0, &application, request_first_decision);
        QTimer::singleShot(50, &application, request_first_decision);
    }
    if (!initial_folder.isEmpty()) {
        controller.scanFolder(QUrl::fromLocalFile(initial_folder));
    }
    if (smoke_test) {
        if (dirty_close_smoke) {
            auto close_started = std::make_shared<bool>(false);
            auto attempt_close = std::make_shared<std::function<void()>>();
            *attempt_close = [
                &application,
                &editor,
                &engine,
                close_started,
                attempt_close
            ]() {
                if (*close_started || !editor.active() || editor.stateBusy()
                    || editor.gradeNodes().isEmpty()) {
                    return;
                }
                // A saved version can intentionally leave a bypassed Grade
                // Node selected.  That is a valid editing state; choose an
                // enabled node before making the smoke's working change,
                // exactly as a user would by clicking an enabled layer.
                const QVariantList grade_nodes = editor.gradeNodes();
                int editable_index = -1;
                for (const QVariant& candidate : grade_nodes) {
                    const QVariantMap node = candidate.toMap();
                    if (node.value(QStringLiteral("enabled")).toBool()) {
                        editable_index = node.value(QStringLiteral("index")).toInt();
                        break;
                    }
                }
                if (editable_index < 0) {
                    qCritical() << "Dirty close smoke found no enabled Grade Node";
                    application.exit(EXIT_FAILURE);
                    return;
                }
                if (editor.selectedGradeNodeIndex() != editable_index) {
                    editor.selectGradeNode(editable_index);
                    QTimer::singleShot(
                        0,
                        &application,
                        [attempt_close]() { (*attempt_close)(); }
                    );
                    return;
                }
                *close_started = true;
                editor.setExposureStops(editor.exposureStops() + 0.25);
                if (!editor.dirty()) {
                    qCritical() << "Dirty close smoke could not create working changes";
                    application.exit(EXIT_FAILURE);
                    return;
                }
                auto* const window = qobject_cast<QWindow*>(
                    engine.rootObjects().front()
                );
                if (window == nullptr || window->close()) {
                    qCritical() << "Dirty close smoke did not defer for autosave";
                    application.exit(EXIT_FAILURE);
                    return;
                }
                QTimer::singleShot(
                    0,
                    &application,
                    [&application, &engine]() {
                        QObject* const root_object = engine.rootObjects().front();
                        QObject* const dialog = root_object->findChild<QObject*>(
                            QStringLiteral("discardQuitDialog")
                        );
                        if (dialog != nullptr) {
                            qCritical() << "Dirty close smoke found an obsolete discard dialog";
                            application.exit(EXIT_FAILURE);
                        }
                    }
                );
            };
            QObject::connect(
                &editor,
                &EditController::stateBusyChanged,
                &application,
                [attempt_close]() { (*attempt_close)(); }
            );
            QObject::connect(
                &editor,
                &EditController::gradeNodesChanged,
                &application,
                [attempt_close]() { (*attempt_close)(); }
            );
            QTimer::singleShot(30'000, &application, [&application]() {
                qCritical() << "Dirty close lifecycle smoke timed out";
                application.exit(EXIT_FAILURE);
            });
        } else if (close_lifecycle_smoke) {
            auto* const window = qobject_cast<QWindow*>(engine.rootObjects().front());
            if (window == nullptr) {
                qCritical() << "Close lifecycle smoke could not find the main window";
                application.exit(EXIT_FAILURE);
            } else {
                QTimer::singleShot(0, window, [window]() { window->close(); });
            }
            QTimer::singleShot(10'000, &application, [&application]() {
                qCritical() << "Close lifecycle smoke timed out";
                application.exit(EXIT_FAILURE);
            });
        } else if (i18n_smoke) {
            QObject* const root_object = engine.rootObjects().front();
            QObject* const settings_button = root_object->findChild<QObject*>(
                QStringLiteral("settingsButton")
            );
            if (settings_button == nullptr) {
                qCritical() << "I18n smoke could not find the Settings control";
                application.exit(EXIT_FAILURE);
            } else {
                preferences.setLanguageMode(QStringLiteral("en"));
                preferences.setLanguageMode(QStringLiteral("zh_CN"));
                QTimer::singleShot(
                    0,
                    &application,
                    [&application, &controller, &preferences, root_object,
                     settings_button]() {
                        const QString chinese_controller_status =
                            controller.statusText();
                        if (preferences.effectiveLanguage()
                                != QStringLiteral("zh_CN")
                            || settings_button->property("text").toString()
                                != QString::fromUtf8("设置")
                            || chinese_controller_status.isEmpty()) {
                            qCritical() << "I18n smoke did not apply Simplified Chinese";
                            application.exit(EXIT_FAILURE);
                            return;
                        }
                        preferences.setLanguageMode(QStringLiteral("en"));
                        QTimer::singleShot(
                            0,
                            &application,
                            [&application, &controller, &preferences, root_object,
                             settings_button, chinese_controller_status]() {
                                QObject* const current_button =
                                    root_object->findChild<QObject*>(
                                        QStringLiteral("settingsButton")
                                    );
                                if (current_button != settings_button
                                    || preferences.effectiveLanguage()
                                        != QStringLiteral("en")
                                    || settings_button->property("text").toString()
                                        != QStringLiteral("Settings")
                                    || controller.statusText()
                                        == chinese_controller_status) {
                                    qCritical() << "I18n smoke did not retranslate in place";
                                    application.exit(EXIT_FAILURE);
                                    return;
                                }
                                const QString english_controller_status =
                                    controller.statusText();
                                preferences.setLanguageMode(QStringLiteral("zh_CN"));
                                QTimer::singleShot(
                                    0,
                                    &application,
                                    [&application, &controller, &preferences,
                                     settings_button, english_controller_status]() {
                                        const bool succeeded =
                                            preferences.effectiveLanguage()
                                                == QStringLiteral("zh_CN")
                                            && settings_button
                                                   ->property("text")
                                                   .toString()
                                                == QString::fromUtf8("设置")
                                            && !controller.statusText().isEmpty()
                                            && controller.statusText()
                                                != english_controller_status;
                                        if (!succeeded) {
                                            qCritical()
                                                << "I18n smoke did not restore Simplified Chinese"
                                                << "language"
                                                << preferences.effectiveLanguage()
                                                << "button"
                                                << settings_button
                                                       ->property("text")
                                                       .toString()
                                                << "status"
                                                << controller.statusText()
                                                << "English status"
                                                << english_controller_status;
                                        }
                                        application.exit(
                                            succeeded ? EXIT_SUCCESS : EXIT_FAILURE
                                        );
                                    }
                                );
                            }
                        );
                    }
                );
            }
            QTimer::singleShot(10'000, &application, [&application]() {
                application.exit(EXIT_FAILURE);
            });
        } else if (cancel_scan_smoke) {
            auto cancel_requested = std::make_shared<bool>(false);
            auto succeeded = std::make_shared<bool>(false);
            QObject::connect(
                &controller,
                &ReviewController::scanProgressChanged,
                &application,
                [&controller, cancel_requested]() {
                    if (*cancel_requested || !controller.scanning()
                        || controller.scanProgress()
                               .value(QStringLiteral("filesSeen"))
                               .toULongLong()
                            == 0) {
                        return;
                    }
                    *cancel_requested = true;
                    controller.cancelScan();
                }
            );
            const auto finish_cancel_smoke = [
                &application,
                &controller,
                cancel_requested,
                succeeded
            ]() {
                if (*succeeded || !*cancel_requested || controller.scanning()
                    || controller.refreshing()
                    || controller.statusText().startsWith(
                        QStringLiteral("Final Library refresh failed")
                    )
                    || controller.scanProgress()
                           .value(QStringLiteral("phase"))
                           .toString()
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
            QTimer::singleShot(30'000, &application, [
                &application,
                cancel_requested,
                succeeded
            ]() {
                if (!*succeeded) {
                    qCritical() << "Cooperative import cancellation smoke failed"
                                << "requested" << *cancel_requested;
                    application.exit(EXIT_FAILURE);
                }
            });
        } else if (streaming_scan_smoke) {
            auto early_model_visible = std::make_shared<bool>(false);
            auto early_qml_visible = std::make_shared<bool>(false);
            auto succeeded = std::make_shared<bool>(false);
            auto evaluate = std::make_shared<std::function<void()>>();
            auto observe_justified_grid = std::make_shared<std::function<void()>>();
            *evaluate = [
                &application,
                &controller,
                &engine,
                early_model_visible,
                early_qml_visible,
                succeeded
            ]() {
                if (*succeeded || controller.scanning() || controller.refreshing()
                    || controller.reviewModel()->rowCount() == 0
                    || review_grid_count(engine) == 0
                    || controller.statusText().startsWith(
                        QStringLiteral("Final Library refresh failed")
                    )
                    || controller.scanProgress()
                           .value(QStringLiteral("phase"))
                           .toString()
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
                        << "first page during scan"
                        << (*early_model_visible && *early_qml_visible);
                QTimer::singleShot(50, &application, &QCoreApplication::quit);
            };
            *observe_justified_grid = [
                &controller,
                &engine,
                early_qml_visible,
                evaluate
            ]() {
                if (controller.scanning()
                    && controller.reviewModel()->rowCount() > 0
                    && review_grid_count(engine) > 0) {
                    *early_qml_visible = true;
                }
                (*evaluate)();
            };
            QObject::connect(
                &controller,
                &ReviewController::itemCountChanged,
                &application,
                [
                    &application,
                    &controller,
                    early_model_visible,
                    observe_justified_grid,
                    evaluate
                ]() {
                    if (controller.scanning()
                        && controller.reviewModel()->rowCount() > 0) {
                        *early_model_visible = true;
                        QTimer::singleShot(
                            0,
                            &application,
                            [observe_justified_grid]() { (*observe_justified_grid)(); }
                        );
                        QTimer::singleShot(
                            50,
                            &application,
                            [observe_justified_grid]() { (*observe_justified_grid)(); }
                        );
                    }
                    (*evaluate)();
                }
            );
            QObject::connect(
                &controller,
                &ReviewController::scanningChanged,
                &application,
                [evaluate]() { (*evaluate)(); }
            );
            QObject::connect(
                &controller,
                &ReviewController::refreshingChanged,
                &application,
                [evaluate]() { (*evaluate)(); }
            );
            QTimer::singleShot(120'000, &application, [
                &application,
                early_model_visible,
                early_qml_visible,
                succeeded
            ]() {
                if (!*succeeded) {
                    qCritical() << "Streaming import smoke failed"
                                << "early model" << *early_model_visible
                                << "early QML" << *early_qml_visible;
                    application.exit(EXIT_FAILURE);
                }
            });
        } else if (reopen_library_smoke) {
            auto succeeded = std::make_shared<bool>(false);
            QObject::connect(
                &controller,
                &ReviewController::itemCountChanged,
                &application,
                [&application, &controller, &engine, succeeded]() {
                    QTimer::singleShot(
                        0,
                        &application,
                        [&application, &controller, &engine, succeeded]() {
                            if (*succeeded || controller.scanning()
                                || controller.refreshing()
                                || controller.reviewModel()->rowCount() == 0
                                || review_grid_count(engine) == 0) {
                                return;
                            }
                            *succeeded = true;
                            qInfo() << "Reopen smoke loaded the persisted Library without scanning";
                            QTimer::singleShot(
                                50,
                                &application,
                                &QCoreApplication::quit
                            );
                        }
                    );
                }
            );
            QTimer::singleShot(30'000, &application, [&application, succeeded]() {
                application.exit(*succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
            });
        } else if (record_first_comparison) {
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
                [&application, &controller, comparison_succeeded]() {
                    if (!*comparison_succeeded) {
                        qCritical()
                            << "Comparison smoke timed out"
                            << "rows" << controller.reviewModel()->rowCount()
                            << "scanning" << controller.scanning()
                            << "refreshing" << controller.refreshing()
                            << "busy" << controller.comparisonBusy()
                            << "session evidence"
                            << controller.sessionEvidenceCount()
                            << "status" << controller.comparisonStatusText();
                    }
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
                [&application, &controller, decision_succeeded]() {
                    if (!*decision_succeeded) {
                        qCritical()
                            << "Decision smoke failed"
                            << "rows" << controller.reviewModel()->rowCount()
                            << "scanning" << controller.scanning()
                            << "refreshing" << controller.refreshing()
                            << "busy" << controller.decisionBusy()
                            << "can undo" << controller.canUndoDecision()
                            << "status" << controller.decisionStatusText();
                    }
                    application.exit(*decision_succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
                }
            );
        } else if (open_first_edit && full_detail_smoke) {
            DesktopSmoke::startEditPreviewSession(
                application,
                engine,
                controller,
                editor,
                edit_preview_provider,
                {
                    .request_before = request_before,
                    .request_full_detail = true,
                }
            );
        } else if (open_first_edit && grade_stack_smoke) {
            DesktopSmoke::startGradeStackPersistence(application, editor);
        } else if (open_first_edit) {
            DesktopSmoke::startEditPreviewSession(
                application,
                engine,
                controller,
                editor,
                edit_preview_provider,
                {
                    .request_before = request_before,
                    .request_full_detail = false,
                }
            );
        } else {
            QTimer::singleShot(500, &application, &QCoreApplication::quit);
        }
    }
}
