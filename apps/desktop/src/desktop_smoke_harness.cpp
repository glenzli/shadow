#include "desktop_smoke_harness.hpp"

#include "desktop_smoke/application_lifecycle.hpp"
#include "desktop_smoke/edit_preview_session.hpp"
#include "desktop_smoke/grade_stack_persistence.hpp"
#include "desktop_smoke/library_lifecycle.hpp"
#include "desktop_smoke/review_mutations.hpp"
#include "desktop_smoke/scenario_setup.hpp"
#include "desktop_smoke/visual_capture.hpp"
#include "edit_controller.hpp"
#include "edit_neighbor_preheater.hpp"
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
    EditNeighborPreheater& edit_neighbor_preheater,
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

    const bool open_first_edit = qEnvironmentVariableIsSet("SHADOW_DESKTOP_OPEN_FIRST_EDIT");
    const bool record_first_comparison =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_RECORD_FIRST_COMPARISON");
    const bool forget_recorded_comparison =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_FORGET_RECORDED_COMPARISON");
    const bool set_first_decision = qEnvironmentVariableIsSet("SHADOW_DESKTOP_SET_FIRST_DECISION");
    const bool undo_first_decision =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_UNDO_FIRST_DECISION");
    const bool before_composition =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_BEFORE_COMPOSITION_SMOKE");
    const bool request_before = before_composition
        || qEnvironmentVariableIsSet("SHADOW_DESKTOP_REQUEST_BEFORE");
    const bool grade_stack_smoke = qEnvironmentVariableIsSet("SHADOW_DESKTOP_GRADE_STACK_SMOKE");
    const bool full_detail_smoke = qEnvironmentVariableIsSet("SHADOW_DESKTOP_FULL_DETAIL_SMOKE");
    const bool metal_preview_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_METAL_PREVIEW_SMOKE");
    const bool software_preview_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_SOFTWARE_PREVIEW_SMOKE");
    const bool rapid_preview_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_RAPID_PREVIEW_SMOKE");
    const bool edit_neighbor_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_EDIT_NEIGHBOR_SMOKE");
    const bool streaming_scan_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_STREAMING_SCAN_SMOKE");
    const bool reopen_library_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_REOPEN_LIBRARY_SMOKE");
    const bool cancel_scan_smoke = qEnvironmentVariableIsSet("SHADOW_DESKTOP_CANCEL_SCAN_SMOKE");
    const bool i18n_smoke = qEnvironmentVariableIsSet("SHADOW_DESKTOP_I18N_SMOKE");
    const bool map_workspace_smoke =
        qEnvironmentVariableIsSet("SHADOW_DESKTOP_MAP_WORKSPACE_SMOKE");
    const bool close_lifecycle_smoke = qEnvironmentVariableIsSet("SHADOW_DESKTOP_CLOSE_SMOKE");
    const bool dirty_close_smoke = qEnvironmentVariableIsSet("SHADOW_DESKTOP_DIRTY_CLOSE_SMOKE");
    const bool smoke_test = qEnvironmentVariableIsSet("SHADOW_DESKTOP_SMOKE_TEST");

    const bool needs_legacy_first_photo_opener =
        !smoke_test || dirty_close_smoke || (grade_stack_smoke && !full_detail_smoke);
    if (open_first_edit && !record_first_comparison && !set_first_decision
        && needs_legacy_first_photo_opener) {
        DesktopSmoke::installFirstPhotoOpener(application, engine, controller, editor);
    }
    if (record_first_comparison) {
        DesktopSmoke::installFirstComparisonRequest(application, controller, thumbnail_provider);
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

    if (qEnvironmentVariableIsSet("SHADOW_DESKTOP_REMOTE_LIBRARY_SMOKE")) {
        DesktopSmoke::startRemoteLibraryLifecycle(application, engine, controller);
    } else if (map_workspace_smoke) {
        QObject* const root = engine.rootObjects().front();
        constexpr int map_workspace_page = 4;
        root->setProperty("workspaceIndex", map_workspace_page);
        QTimer::singleShot(0, &application, [&application, root]() {
            QObject* const map_workspace =
                root->findChild<QObject*>(QStringLiteral("libraryMapWorkspace"));
            QObject* const scope_sidebar =
                root->findChild<QObject*>(QStringLiteral("mapLibraryScopeSidebar"));
            QObject* const scope_button =
                root->findChild<QObject*>(QStringLiteral("mapLibraryScopeButton"));
            if (root->property("workspaceIndex").toInt() != map_workspace_page
                || map_workspace == nullptr || !map_workspace->property("visible").toBool()
                || map_workspace->property("libraryScopeExpanded").toBool()
                || scope_sidebar == nullptr || scope_sidebar->property("visible").toBool()
                || scope_button == nullptr
                || !QMetaObject::invokeMethod(scope_button, "clicked", Qt::DirectConnection)) {
                qCritical() << "Packaged Map workspace did not start with a collapsed scope";
                application.exit(EXIT_FAILURE);
                return;
            }
            QCoreApplication::processEvents();
            if (!map_workspace->property("libraryScopeExpanded").toBool()
                || !scope_sidebar->property("visible").toBool()) {
                qCritical() << "Map scope control did not reveal shared Library navigation";
                application.exit(EXIT_FAILURE);
                return;
            }
            application.quit();
        });
    } else if (dirty_close_smoke) {
        DesktopSmoke::startDirtyCloseLifecycle(application, engine, editor);
    } else if (close_lifecycle_smoke) {
        DesktopSmoke::startCloseLifecycle(application, engine);
    } else if (i18n_smoke) {
        DesktopSmoke::startI18nLifecycle(application, engine, controller, preferences);
    } else if (cancel_scan_smoke) {
        DesktopSmoke::startCancelScanLifecycle(application, controller);
    } else if (streaming_scan_smoke) {
        DesktopSmoke::startStreamingScanLifecycle(application, engine, controller, editor);
    } else if (reopen_library_smoke) {
        DesktopSmoke::startReopenLibraryLifecycle(application, engine, controller);
    } else if (record_first_comparison) {
        DesktopSmoke::awaitFirstComparisonMutation(
            application,
            controller,
            forget_recorded_comparison
        );
    } else if (set_first_decision) {
        DesktopSmoke::awaitFirstDecisionMutation(application, controller, undo_first_decision);
    } else if (open_first_edit && full_detail_smoke) {
        DesktopSmoke::startEditPreviewSession(
            application,
            engine,
            controller,
            editor,
            edit_preview_provider,
            {
                .request_before = request_before,
                .before_composition = before_composition,
                .request_full_detail = true,
                .rapid_parameter_updates = rapid_preview_smoke,
                .edit_neighbor_preheater = &edit_neighbor_preheater,
                .transport_expectation =
                    metal_preview_smoke ? DesktopSmoke::EditPreviewTransportExpectation::MetalNative
                    : software_preview_smoke
                        ? DesktopSmoke::EditPreviewTransportExpectation::SoftwareFallback
                        : DesktopSmoke::EditPreviewTransportExpectation::None,
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
                .before_composition = before_composition,
                .request_full_detail = false,
                .rapid_parameter_updates = rapid_preview_smoke,
                .edit_neighbor_preheater = &edit_neighbor_preheater,
                .verify_adjacent_edit = edit_neighbor_smoke,
                .transport_expectation =
                    metal_preview_smoke ? DesktopSmoke::EditPreviewTransportExpectation::MetalNative
                    : software_preview_smoke
                        ? DesktopSmoke::EditPreviewTransportExpectation::SoftwareFallback
                        : DesktopSmoke::EditPreviewTransportExpectation::None,
            }
        );
    } else {
        QTimer::singleShot(500, &application, &QCoreApplication::quit);
    }
}
