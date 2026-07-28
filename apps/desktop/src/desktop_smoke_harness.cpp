#include "desktop_smoke_harness.hpp"

#include "desktop_smoke/application_lifecycle.hpp"
#include "desktop_smoke/edit_preview_session.hpp"
#include "desktop_smoke/grade_stack_persistence.hpp"
#include "desktop_smoke/library_lifecycle.hpp"
#include "desktop_smoke/review_mutations.hpp"
#include "desktop_smoke/scenario_setup.hpp"
#include "desktop_smoke/visual_capture.hpp"
#include "edit_controller.hpp"
#include "review_controller.hpp"
#include "ui_preferences.hpp"

#include <QApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QUrl>

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
    DesktopSmoke::installVisualCapture(application, engine);

    if (qEnvironmentVariableIsSet("SHADOW_DESKTOP_OPEN_LUT_LIBRARY")) {
        QMetaObject::invokeMethod(
            engine.rootObjects().front(),
            "openLutManager",
            Qt::DirectConnection
        );
    }

    const bool open_first_edit =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_OPEN_FIRST_EDIT");
    const bool record_first_comparison =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_RECORD_FIRST_COMPARISON");
    const bool forget_recorded_comparison =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_FORGET_RECORDED_COMPARISON");
    const bool set_first_decision =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_SET_FIRST_DECISION");
    const bool undo_first_decision =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_UNDO_FIRST_DECISION");
    const bool request_before =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_REQUEST_BEFORE");
    const bool grade_stack_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_GRADE_STACK_SMOKE");
    const bool full_detail_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_FULL_DETAIL_SMOKE");
    const bool streaming_scan_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_STREAMING_SCAN_SMOKE");
    const bool reopen_library_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_REOPEN_LIBRARY_SMOKE");
    const bool cancel_scan_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_CANCEL_SCAN_SMOKE");
    const bool i18n_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_I18N_SMOKE");
    const bool close_lifecycle_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_CLOSE_SMOKE");
    const bool dirty_close_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_DIRTY_CLOSE_SMOKE");
    const bool smoke_test =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_SMOKE_TEST");

    const bool needs_legacy_first_photo_opener =
        !smoke_test || dirty_close_smoke
        || (grade_stack_smoke && !full_detail_smoke);
    if (open_first_edit && !record_first_comparison && !set_first_decision
        && needs_legacy_first_photo_opener) {
        DesktopSmoke::installFirstPhotoOpener(
            application,
            engine,
            controller,
            editor
        );
    }
    if (record_first_comparison) {
        DesktopSmoke::installFirstComparisonRequest(
            application,
            controller,
            thumbnail_provider
        );
    }
    if (set_first_decision && !record_first_comparison) {
        DesktopSmoke::installFirstDecisionRequest(application, controller);
    }
    if (!initial_folder.isEmpty()) {
        controller.scanFolder(QUrl::fromLocalFile(initial_folder));
    }
    if (!smoke_test) {
        return;
    }

    if (dirty_close_smoke) {
        DesktopSmoke::startDirtyCloseLifecycle(application, engine, editor);
    } else if (close_lifecycle_smoke) {
        DesktopSmoke::startCloseLifecycle(application, engine);
    } else if (i18n_smoke) {
        DesktopSmoke::startI18nLifecycle(
            application,
            engine,
            controller,
            preferences
        );
    } else if (cancel_scan_smoke) {
        DesktopSmoke::startCancelScanLifecycle(application, controller);
    } else if (streaming_scan_smoke) {
        DesktopSmoke::startStreamingScanLifecycle(
            application,
            engine,
            controller
        );
    } else if (reopen_library_smoke) {
        DesktopSmoke::startReopenLibraryLifecycle(
            application,
            engine,
            controller
        );
    } else if (record_first_comparison) {
        DesktopSmoke::awaitFirstComparisonMutation(
            application,
            controller,
            forget_recorded_comparison
        );
    } else if (set_first_decision) {
        DesktopSmoke::awaitFirstDecisionMutation(
            application,
            controller,
            undo_first_decision
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
