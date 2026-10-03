#include "pipeline_run_controller.hpp"
#include "pipeline_smoke_harness.hpp"
#include "subject_emphasis_smoke_harness.hpp"

#include "ai_preferences.hpp"
#include "backend/export_backend.hpp"
#include "backend/export_settings_codec.hpp"
#include "desktop_backend.hpp"
#include "edit_controller.hpp"
#include "edit_interchange_controller.hpp"
#include "edit_preview_presentation_context.hpp"
#include "edit_preview_presentation_registry.hpp"
#include "edit_preview_provider.hpp"
#include "edit_tool_controller.hpp"
#include "edit_tool_stdio.hpp"
#include "edit_tool_ui_smoke.hpp"
#include "export_task_runner.hpp"
#include "lut_library.hpp"
#include "lut_preview_provider.hpp"
#include "optics_profile_library.hpp"
#include "ui_preferences.hpp"

#if defined(Q_OS_MACOS)
#include "platform/macos/titlebar.hpp"
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>

#include <cstdlib>
#include <exception>
#include <utility>

namespace {
void configurePipelineDecodeHelper() {
    const QString helper_path = QDir(QCoreApplication::applicationDirPath())
                                    .filePath(QStringLiteral("shadow-image-decode-helper"));
    qputenv("SHADOW_DISABLE_PRIVATE_DECODER", QByteArrayLiteral("1"));
    if (QFileInfo(helper_path).isExecutable()) {
        qputenv("SHADOW_DECODE_HELPER_PATH", helper_path.toUtf8());
    }
}

} // namespace

int runPipelineEdit(QApplication& application, const PipelineLaunchRequest& request) {
    QCoreApplication::setOrganizationName(QStringLiteral("Shadow"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("shadow.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("Shadow Pipeline"));
    configurePipelineDecodeHelper();
    // Explicitly share the installed application's service identity, while all
    // catalog, Recipe, derived-raster and preference state remains temporary.
    // Never copy credentials into the session directory or widen its ACL.
    if (qEnvironmentVariableIsEmpty("SHADOW_INFER_CREDENTIAL_FILE")) {
        const QString credential =
            QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                .filePath(QStringLiteral("Shadow/credentials/infer-runtime-shadow.token"));
        qputenv("SHADOW_INFER_CREDENTIAL_FILE", credential.toUtf8());
    }

    QTemporaryDir runtime_root;
    if (!runtime_root.isValid()) {
        qCritical() << "Cannot create private pipeline runtime directory";
        (void)writePipelineResult(
            request,
            QStringLiteral("failed"),
            {QStringLiteral("private pipeline runtime unavailable")}
        );
        return EXIT_FAILURE;
    }
    const int exit_code = [&]() -> int {
        const QString catalog_path =
            QDir(runtime_root.path()).filePath(QStringLiteral("catalog.sqlite"));
        const QString cache_root = QDir(runtime_root.path()).filePath(QStringLiteral("cache"));
        const QString settings_path = QDir(runtime_root.path()).filePath(QStringLiteral("ui.ini"));

        std::shared_ptr<DesktopBackend> backend;
        try {
            backend = std::make_shared<DesktopBackend>(catalog_path, cache_root);
        } catch (const std::exception& exception) {
            qCritical().noquote() << "Cannot start pipeline backend:" << exception.what();
            (void)writePipelineResult(
                request,
                QStringLiteral("failed"),
                {QString::fromUtf8(exception.what())}
            );
            return EXIT_FAILURE;
        }
        UiPreferences preferences(application, settings_path);
        AiPreferences ai_preferences(runtime_root.path(), settings_path);
        LutLibrary lut_library(
            settings_path,
            QDir(runtime_root.path()).filePath(QStringLiteral("luts"))
        );
        OpticsProfileLibrary optics_profile_library(
            QDir(runtime_root.path()).filePath(QStringLiteral("optics-profiles"))
        );
        auto preview_store = std::make_shared<EditPreviewStore>();
        auto preview_context = std::make_shared<EditPreviewPresentationContext>();
        EditPreviewPresentationRegistry preview_presentation(preview_store, preview_context);
        EditController editor(backend, preview_store, preview_context, &ai_preferences);
        EditInterchangeController interchange(*backend, editor);
        PipelineRunController pipeline(backend, editor, request);
        std::unique_ptr<EditToolStdio> tool_stdio;
        std::unique_ptr<EditToolController> tools;
        if (request.agent_stdio) {
            QThreadPool::globalInstance()->setMaxThreadCount(2);
            try {
                tool_stdio = std::make_unique<EditToolStdio>();
                tools = std::make_unique<EditToolController>(
                    editor,
                    backend,
                    request.photos.front().input_path
                );
            } catch (const std::exception& error) {
                qCritical().noquote() << "Cannot start edit tools:" << error.what();
                return EXIT_FAILURE;
            }
            QObject::connect(&pipeline, &PipelineRunController::changed, tools.get(), [&] {
                tools->setAdmissionError(
                    !pipeline.busy() && !editor.active() ? pipeline.errorText() : QString{}
                );
            });
            QObject::connect(
                tool_stdio.get(),
                &EditToolStdio::lineReceived,
                tools.get(),
                &EditToolController::receive
            );
            QObject::connect(
                tools.get(),
                &EditToolController::reply,
                tool_stdio.get(),
                &EditToolStdio::send
            );
            QObject::connect(
                tools.get(),
                &EditToolController::shutdownRequested,
                tool_stdio.get(),
                &EditToolStdio::finish
            );
            QObject::connect(
                tool_stdio.get(),
                &EditToolStdio::drained,
                &application,
                &QCoreApplication::quit
            );
            QObject::connect(
                tool_stdio.get(),
                &EditToolStdio::inputClosed,
                tools.get(),
                &EditToolController::disconnectClient
            );
        }

        QQmlApplicationEngine engine;
        preferences.attachEngine(engine);
        engine.addImageProvider(
            QStringLiteral("shadow-edit"),
            new EditPreviewProvider(preview_store, preview_context)
        );
        engine.addImageProvider(
            QStringLiteral("shadow-lut"),
            new LutPreviewProvider(
                QDir(runtime_root.path()).filePath(QStringLiteral("luts")),
                QDir(cache_root).filePath(QStringLiteral("lut-previews"))
            )
        );
        engine.setInitialProperties({
            {QStringLiteral("editor"), QVariant::fromValue(&editor)},
            {QStringLiteral("preferences"), QVariant::fromValue(&preferences)},
            {QStringLiteral("interchangeController"), QVariant::fromValue(&interchange)},
            {QStringLiteral("editPreviewPresentation"), QVariant::fromValue(&preview_presentation)},
            {QStringLiteral("lutLibrary"), QVariant::fromValue(&lut_library)},
            {QStringLiteral("opticsProfileLibrary"), QVariant::fromValue(&optics_profile_library)},
            {QStringLiteral("pipeline"), QVariant::fromValue(&pipeline)},
        });
        engine.loadFromModule("Shadow.App", "PipelineEditor");
        if (engine.rootObjects().isEmpty()) {
            (void)writePipelineResult(
                request,
                QStringLiteral("failed"),
                {QStringLiteral("pipeline editor could not start")}
            );
            return EXIT_FAILURE;
        }
        auto* const root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        if (root_window == nullptr) {
            qCritical() << "PipelineEditor root must be a QQuickWindow";
            (void)writePipelineResult(
                request,
                QStringLiteral("failed"),
                {QStringLiteral("pipeline editor root is invalid")}
            );
            return EXIT_FAILURE;
        }
        preview_context->attach(root_window);
#if defined(Q_OS_MACOS)
        const auto* const title_bar =
            root_window->findChild<QObject*>(QStringLiteral("titleToolBar"));
        installMacTitleBarAlignment(
            root_window,
            title_bar == nullptr ? 44 : qRound(title_bar->property("height").toReal())
        );
#endif
        if (!request.agent_stdio) {
            if (qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_ACTION") == "agent-tools"
                && request.photos.size() == 1) {
                installEditToolUiSmoke(
                    engine,
                    pipeline,
                    editor,
                    backend,
                    request.photos.front().input_path
                );
            } else {
                installPipelineSmokeHarness(engine, pipeline, editor, ai_preferences);
                installSubjectEmphasisSmokeHarness(engine, pipeline, editor);
            }
        }
        QTimer::singleShot(0, &pipeline, &PipelineRunController::start);
        return application.exec();
    }();
    if (!runtime_root.remove()) {
        qWarning() << "Could not remove private pipeline runtime";
    }
    return exit_code;
}
