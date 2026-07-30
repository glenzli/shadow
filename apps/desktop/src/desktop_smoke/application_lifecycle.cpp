#include "application_lifecycle.hpp"

#include "../edit_controller.hpp"
#include "../review_controller.hpp"
#include "../ui_preferences.hpp"

#include <QApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QVariant>
#include <QWindow>

#include <functional>
#include <memory>

namespace DesktopSmoke {

namespace {

[[nodiscard]] bool isEnglishLibraryEmptyState(const QString& text) {
    return text
            == QStringLiteral(
                "Searching the folder for supported photos…\n"
                "New RAW files will appear here as they are catalogued."
            )
        || text
            == QStringLiteral(
                "Import stopped, and no RAW files are currently visible.\n"
                "Already catalogued files remain safely stored."
            )
        || text
            == QStringLiteral(
                "Add a folder to the local Library.\n"
                "Shadow will show embedded previews immediately, "
                "then replace them with locally generated proxies."
            );
}

[[nodiscard]] bool isChineseLibraryEmptyState(const QString& text) {
    return text
            == QString::fromUtf8(
                "正在文件夹中搜索支持的照片…\n"
                "新的 RAW 文件会在收录后显示于此。"
            )
        || text
            == QString::fromUtf8(
                "导入已停止，目前没有可见的 RAW 文件。\n"
                "已收录的文件仍安全存储。"
            )
        || text
            == QString::fromUtf8(
                "向本地图库添加文件夹。\n"
                "Shadow 会立即显示内嵌预览，随后以本地生成的代理替换。"
            );
}

} // namespace

void startDirtyCloseLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    EditController& editor
) {
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
}

void startCloseLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine
) {
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
}

void startI18nLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller,
    UiPreferences& preferences
) {
    QObject* const root_object = engine.rootObjects().front();
    QObject* const settings_button = root_object->findChild<QObject*>(
        QStringLiteral("settingsButton")
    );
    QObject* const empty_state_text = root_object->findChild<QObject*>(
        QStringLiteral("reviewEmptyStateText")
    );
    QObject* const analysis_title = root_object->findChild<QObject*>(
        QStringLiteral("analysisScopeTitleText")
    );
    QObject* const analysis_shadow_summary = root_object->findChild<QObject*>(
        QStringLiteral("analysisShadowSummaryText")
    );
    QObject* const capture_metadata_text = root_object->findChild<QObject*>(
        QStringLiteral("precisionCaptureMetadataText")
    );
    QObject* const precision_rendering_status = root_object->findChild<QObject*>(
        QStringLiteral("precisionRenderingStatusText")
    );
    if (settings_button == nullptr || empty_state_text == nullptr
        || analysis_title == nullptr || analysis_shadow_summary == nullptr
        || capture_metadata_text == nullptr
        || precision_rendering_status == nullptr) {
        qCritical() << "I18n smoke could not find its translated controls";
        application.exit(EXIT_FAILURE);
    } else {
        preferences.setLanguageMode(QStringLiteral("en"));
        preferences.setLanguageMode(QStringLiteral("zh_CN"));
        QTimer::singleShot(
            0,
            &application,
            [&application, &controller, &preferences, root_object,
             settings_button, empty_state_text, analysis_title,
             analysis_shadow_summary, capture_metadata_text,
             precision_rendering_status]() {
                const QString chinese_controller_status =
                    controller.statusText();
                if (preferences.effectiveLanguage()
                        != QStringLiteral("zh_CN")
                    || settings_button->property("text").toString()
                        != QString::fromUtf8("设置")
                    || !isChineseLibraryEmptyState(
                        empty_state_text->property("text").toString()
                    )
                    || analysis_title->property("text").toString()
                        != QString::fromUtf8("图像分析")
                    || analysis_shadow_summary->property("text").toString()
                        != QString::fromUtf8("阴影  —")
                    || (capture_metadata_text->property("text").toString()
                            != QString::fromUtf8("正在准备拍摄元数据…")
                        && capture_metadata_text->property("text").toString()
                            != QString::fromUtf8("拍摄元数据不可用"))
                    || precision_rendering_status
                           ->property("text")
                           .toString()
                        != QString::fromUtf8("正在打开照片")
                    || chinese_controller_status.isEmpty()) {
                    qCritical()
                        << "I18n smoke did not apply Simplified Chinese"
                        << "language" << preferences.effectiveLanguage()
                        << "settings" << settings_button->property("text")
                        << "library" << empty_state_text->property("text")
                        << "analysis" << analysis_title->property("text")
                        << "shadows"
                        << analysis_shadow_summary->property("text")
                        << "metadata"
                        << capture_metadata_text->property("text")
                        << "Precision status"
                        << precision_rendering_status->property("text")
                        << "status" << chinese_controller_status;
                    application.exit(EXIT_FAILURE);
                    return;
                }
                preferences.setLanguageMode(QStringLiteral("en"));
                QTimer::singleShot(
                    0,
                    &application,
                    [&application, &controller, &preferences, root_object,
                     settings_button, empty_state_text,
                     analysis_title, analysis_shadow_summary,
                     capture_metadata_text, precision_rendering_status,
                     chinese_controller_status]() {
                        QObject* const current_button =
                            root_object->findChild<QObject*>(
                                QStringLiteral("settingsButton")
                            );
                        QObject* const current_empty_state =
                            root_object->findChild<QObject*>(
                                QStringLiteral("reviewEmptyStateText")
                            );
                        if (current_button != settings_button
                            || current_empty_state != empty_state_text
                            || preferences.effectiveLanguage()
                                != QStringLiteral("en")
                            || settings_button->property("text").toString()
                                != QStringLiteral("Settings")
                            || !isEnglishLibraryEmptyState(
                                empty_state_text->property("text").toString()
                            )
                            || analysis_title->property("text").toString()
                                != QStringLiteral("ANALYSIS")
                            || analysis_shadow_summary
                                   ->property("text")
                                   .toString()
                                != QStringLiteral("SHADOWS  —")
                            || (capture_metadata_text
                                    ->property("text")
                                    .toString()
                                    != QStringLiteral(
                                        "Preparing capture metadata…")
                                && capture_metadata_text
                                       ->property("text")
                                       .toString()
                                    != QStringLiteral(
                                        "Capture metadata unavailable"))
                            || precision_rendering_status
                                   ->property("text")
                                   .toString()
                                != QStringLiteral("Opening photo")
                            || controller.statusText()
                                == chinese_controller_status) {
                            qCritical()
                                << "I18n smoke did not retranslate in place"
                                << "language"
                                << preferences.effectiveLanguage()
                                << "settings"
                                << settings_button->property("text")
                                << "library"
                                << empty_state_text->property("text")
                                << "analysis"
                                << analysis_title->property("text")
                                << "shadows"
                                << analysis_shadow_summary->property("text")
                                << "metadata"
                                << capture_metadata_text->property("text")
                                << "Precision status"
                                << precision_rendering_status
                                       ->property("text")
                                << "status" << controller.statusText()
                                << "Chinese status"
                                << chinese_controller_status;
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
                             settings_button, empty_state_text,
                             analysis_title, analysis_shadow_summary,
                             capture_metadata_text,
                             precision_rendering_status,
                             english_controller_status]() {
                                const bool succeeded =
                                    preferences.effectiveLanguage()
                                        == QStringLiteral("zh_CN")
                                    && settings_button
                                           ->property("text")
                                           .toString()
                                        == QString::fromUtf8("设置")
                                    && isChineseLibraryEmptyState(
                                        empty_state_text
                                            ->property("text")
                                            .toString()
                                    )
                                    && analysis_title
                                           ->property("text")
                                           .toString()
                                        == QString::fromUtf8("图像分析")
                                    && analysis_shadow_summary
                                           ->property("text")
                                           .toString()
                                        == QString::fromUtf8("阴影  —")
                                    && (capture_metadata_text
                                            ->property("text")
                                            .toString()
                                            == QString::fromUtf8(
                                                "正在准备拍摄元数据…")
                                        || capture_metadata_text
                                               ->property("text")
                                               .toString()
                                            == QString::fromUtf8(
                                                "拍摄元数据不可用"))
                                    && precision_rendering_status
                                           ->property("text")
                                           .toString()
                                        == QString::fromUtf8(
                                            "正在打开照片")
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
}

} // namespace DesktopSmoke
