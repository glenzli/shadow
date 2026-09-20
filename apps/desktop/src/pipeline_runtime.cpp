#include "pipeline_run_controller.hpp"
#include "pipeline_smoke_harness.hpp"

#include "ai_preferences.hpp"
#include "backend/export_backend.hpp"
#include "backend/export_settings_codec.hpp"
#include "desktop_backend.hpp"
#include "edit_controller.hpp"
#include "edit_interchange_controller.hpp"
#include "edit_preview_presentation_context.hpp"
#include "edit_preview_presentation_registry.hpp"
#include "edit_preview_provider.hpp"
#include "export_task_runner.hpp"
#include "lut_library.hpp"
#include "lut_preview_provider.hpp"
#include "optics_profile_library.hpp"
#include "ui_preferences.hpp"

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
#include <QTemporaryDir>
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
        installPipelineSmokeHarness(engine, pipeline, editor);
        QTimer::singleShot(0, &pipeline, &PipelineRunController::start);
        return application.exec();
    }();
    if (!runtime_root.remove()) {
        qWarning() << "Could not remove private pipeline runtime";
    }
    return exit_code;
}
