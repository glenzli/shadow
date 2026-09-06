#include "pipeline_run_controller.hpp"

#include "ai_preferences.hpp"
#include "backend/export_backend.hpp"
#include "backend/export_settings_codec.hpp"
#include "desktop_backend.hpp"
#include "edit_controller.hpp"
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

bool writePipelineResult(
    const PipelineLaunchRequest& request,
    const QString& outcome,
    const QStringList& errors
) {
    if (QFileInfo::exists(request.result_path)) {
        return false;
    }
    QJsonArray error_values;
    for (const QString& error : errors) {
        error_values.append(error);
    }
    const QJsonObject result{
        {QStringLiteral("schema"), QStringLiteral("shadow-pipeline-result-20260814.1")},
        {QStringLiteral("requestId"), request.request_id},
        {QStringLiteral("outcome"), outcome},
        {QStringLiteral("input"), request.input_path},
        {QStringLiteral("output"), request.output_path},
        {QStringLiteral("errors"), error_values},
    };
    QSaveFile file(request.result_path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly)
           && file.write(QJsonDocument(result).toJson(QJsonDocument::Compact)) >= 0
           && file.commit();
}

} // namespace

PipelineRunController::PipelineRunController(
    std::shared_ptr<DesktopBackend> backend,
    EditController& editor,
    PipelineLaunchRequest request,
    QObject* parent
) : QObject(parent), backend_(std::move(backend)), editor_(editor), request_(std::move(request)) {
    connect(
        &export_watcher_,
        &QFutureWatcher<ExportTaskResult>::finished,
        this,
        &PipelineRunController::finishExport
    );
    connect(&editor_, &EditController::activeChanged, this, [this] {
        if (completion_requested_ && !editor_.active()) {
            QTimer::singleShot(0, this, &PipelineRunController::beginExport);
        }
    });
}

PipelineRunController::~PipelineRunController() {
    if (cancellation_token_) {
        cancellation_token_->store(true, std::memory_order_relaxed);
    }
    export_watcher_.waitForFinished();
}

bool PipelineRunController::busy() const noexcept {
    return !terminal_ && (export_watcher_.isRunning() || completion_requested_ || editor_.busy());
}

QString PipelineRunController::statusText() const {
    return status_text_.isEmpty() ? editor_.statusText() : status_text_;
}

void PipelineRunController::start() {
    if (started_ || terminal_) {
        return;
    }
    started_ = true;
    try {
        const DesktopBackend::PipelineInput input =
            backend_->admitPipelineInput(request_.input_path);
        photo_id_ = input.photo_id;
        representation_id_ = input.representation_id;
        source_path_ = input.source_path;
        title_ = input.title;
        if (!editor_.openPhoto(photo_id_, representation_id_, source_path_, title_)) {
            finish(QStringLiteral("failed"), EXIT_FAILURE, {editor_.statusText()});
            return;
        }
    } catch (const std::exception& exception) {
        finish(QStringLiteral("failed"), EXIT_FAILURE, {QString::fromUtf8(exception.what())});
        return;
    }
    emit changed();
}

void PipelineRunController::complete() {
    if (terminal_ || !started_ || completion_requested_ || export_watcher_.isRunning()) {
        return;
    }
    completion_requested_ = true;
    status_text_.clear();
    editor_.closePhoto();
    if (!editor_.active()) {
        QTimer::singleShot(0, this, &PipelineRunController::beginExport);
    }
    emit changed();
}

void PipelineRunController::cancel() {
    if (terminal_) {
        return;
    }
    if (cancellation_token_) {
        cancellation_token_->store(true, std::memory_order_relaxed);
        status_text_.clear();
        emit changed();
        return;
    }
    finish(QStringLiteral("cancelled"), 2);
}

void PipelineRunController::beginExport() {
    if (terminal_ || !completion_requested_ || export_watcher_.isRunning() || editor_.active()
        || editor_.busy()) {
        if (!terminal_ && completion_requested_ && (editor_.active() || editor_.busy())) {
            QTimer::singleShot(25, this, &PipelineRunController::beginExport);
        }
        return;
    }
    try {
        const BackendExportOptions options =
            ExportSettingsCodec::fromVariantMap(request_.export_options);
        const auto export_backend =
            std::shared_ptr<ExportBackend>(backend_, &backend_->exportBackend());
        cancellation_token_ = std::make_shared<std::atomic_bool>(false);
        const QVector<BackendDurableExportTarget> targets{{
            .photo_id = photo_id_,
            .source_path = source_path_,
            .output_path = request_.output_path,
        }};
        const QString settings_json = ExportSettingsCodec::toDurableJson(options);
        status_text_.clear();
        export_watcher_.setFuture(
            QtConcurrent::run(
                ExportTaskRunner::runDurableExport,
                export_backend,
                targets,
                settings_json,
                false,
                cancellation_token_,
                ExportProgressReporter{}
            )
        );
    } catch (const std::exception& exception) {
        finish(QStringLiteral("failed"), EXIT_FAILURE, {QString::fromUtf8(exception.what())});
        return;
    }
    emit changed();
}

void PipelineRunController::finishExport() {
    const ExportTaskResult result = export_watcher_.result();
    cancellation_token_.reset();
    if (result.completed == 1 && result.failed == 0 && !result.cancelled) {
        finish(QStringLiteral("completed"), EXIT_SUCCESS);
    } else if (result.cancelled) {
        finish(QStringLiteral("cancelled"), 2, result.errors);
    } else {
        finish(QStringLiteral("failed"), EXIT_FAILURE, result.errors);
    }
}

void PipelineRunController::finish(
    const QString& outcome,
    const int exit_code,
    const QStringList& errors
) {
    if (terminal_) {
        return;
    }
    if (!writePipelineResult(request_, outcome, errors)) {
        terminal_ = true;
        emit changed();
        QCoreApplication::exit(EXIT_FAILURE);
        return;
    }
    terminal_ = true;
    emit changed();
    QCoreApplication::exit(exit_code);
}

int runPipelineEdit(QApplication& application, const PipelineLaunchRequest& request) {
    QCoreApplication::setOrganizationName(QStringLiteral("Shadow"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("shadow.dev"));
    QCoreApplication::setApplicationName(QStringLiteral("Shadow Pipeline"));
    configurePipelineDecodeHelper();

    QTemporaryDir runtime_root;
    if (!runtime_root.isValid()) {
        qCritical() << "Cannot create private pipeline runtime directory";
        writePipelineResult(
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
            writePipelineResult(
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
            {QStringLiteral("editPreviewPresentation"), QVariant::fromValue(&preview_presentation)},
            {QStringLiteral("lutLibrary"), QVariant::fromValue(&lut_library)},
            {QStringLiteral("opticsProfileLibrary"), QVariant::fromValue(&optics_profile_library)},
            {QStringLiteral("pipeline"), QVariant::fromValue(&pipeline)},
        });
        engine.loadFromModule("Shadow.App", "PipelineEditor");
        if (engine.rootObjects().isEmpty()) {
            writePipelineResult(
                request,
                QStringLiteral("failed"),
                {QStringLiteral("pipeline editor could not start")}
            );
            return EXIT_FAILURE;
        }
        auto* const root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        if (root_window == nullptr) {
            qCritical() << "PipelineEditor root must be a QQuickWindow";
            writePipelineResult(
                request,
                QStringLiteral("failed"),
                {QStringLiteral("pipeline editor root is invalid")}
            );
            return EXIT_FAILURE;
        }
        preview_context->attach(root_window);
        QTimer::singleShot(0, &pipeline, &PipelineRunController::start);
        return application.exec();
    }();
    if (!runtime_root.remove()) {
        qWarning() << "Could not remove private pipeline runtime";
    }
    return exit_code;
}
