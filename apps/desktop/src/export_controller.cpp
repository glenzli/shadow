#include "export_controller.hpp"

#include "backend/export_backend.hpp"
#include "backend/export_settings_codec.hpp"
#include "desktop_backend.hpp"
#include "export_preset_store.hpp"
#include "export_watermark_store.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] int checked_export_count(const qsizetype count) {
    if (count < 0 || count > std::numeric_limits<int>::max()) {
        throw std::length_error("export selection exceeds the desktop count limit");
    }
    return static_cast<int>(count);
}

struct ExportPhoto final {
    QString photo_id;
    QString source_path;
    QString title;
};

[[nodiscard]] QString safe_stem(const QString& requested, const QString& source_path) {
    QString stem = requested.trimmed();
    if (stem.isEmpty()) {
        stem = QFileInfo(source_path).completeBaseName();
    }
    stem.replace(
        QRegularExpression(QStringLiteral(R"([/\\:\*\?"<>\|])")),
        QStringLiteral("_")
    );
    stem = stem.trimmed();
    return stem.isEmpty() ? QStringLiteral("Shadow export") : stem;
}

[[nodiscard]] QString unique_destination(
    const QDir& folder,
    const QString& stem,
    const QString& suffix,
    const QString& extension,
    QSet<QString>& reserved_destinations
) {
    const QString base = stem + suffix;
    QString candidate = folder.filePath(base + QStringLiteral(".") + extension);
    for (int copy = 2;
         QFileInfo::exists(candidate) || reserved_destinations.contains(candidate);
         ++copy) {
        candidate = folder.filePath(
            QStringLiteral("%1-%2.%3").arg(base).arg(copy).arg(extension)
        );
    }
    reserved_destinations.insert(candidate);
    return candidate;
}

[[nodiscard]] std::shared_ptr<ExportBackend> shared_export_backend(
    const std::shared_ptr<DesktopBackend>& backend
) {
    if (!backend) {
        throw std::invalid_argument("ExportController requires a desktop backend");
    }
    return std::shared_ptr<ExportBackend>(backend, &backend->exportBackend());
}

[[nodiscard]] LocalizedUiMessage export_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ExportController", source, arguments};
}

} // namespace

ExportController::ExportController(
    std::shared_ptr<DesktopBackend> backend,
    const QString& isolated_settings_file,
    QObject* parent
)
    : QObject(parent),
      export_backend_(shared_export_backend(backend)),
      preset_store_(
          std::make_unique<ExportPresetStore>(isolated_settings_file)
      ),
      watermark_store_(
          std::make_unique<ExportWatermarkStore>(isolated_settings_file)
      ) {
    connect(
        &watcher_,
        &QFutureWatcher<ExportTaskResult>::finished,
        this,
        &ExportController::finishExport
    );
    // Recovery only touches the catalog from the background worker. It does
    // not make the first Library frame wait on SQLite or a source decode.
    QTimer::singleShot(0, this, &ExportController::startRecoveryDrain);
    if (auto* const application = QCoreApplication::instance()) {
        application->installEventFilter(this);
    }
}

ExportController::~ExportController() {
    if (cancellation_token_) {
        cancellation_token_->store(true, std::memory_order_relaxed);
    }
    watcher_.waitForFinished();
}

bool ExportController::busy() const noexcept {
    return watcher_.isRunning();
}

QString ExportController::statusText() const {
    return raw_status_text_.isEmpty()
        ? status_message_.translated()
        : raw_status_text_;
}

int ExportController::completedCount() const noexcept {
    return completed_count_;
}

int ExportController::failedCount() const noexcept {
    return failed_count_;
}

int ExportController::currentCount() const noexcept {
    return current_count_;
}

int ExportController::totalCount() const noexcept {
    return total_count_;
}

bool ExportController::cancellationRequested() const noexcept {
    return cancellation_requested_;
}

QStringList ExportController::errors() const {
    return errors_;
}

QVariantList ExportController::presets() const {
    return preset_store_->presets();
}

QVariantList ExportController::watermarks() const {
    return watermark_store_->watermarks();
}

bool ExportController::eventFilter(
    QObject* const watched,
    QEvent* const event
) {
    if (watched == QCoreApplication::instance()
        && event->type() == QEvent::LanguageChange) {
        if (raw_status_text_.isEmpty() && !status_message_.isEmpty()) {
            emit statusTextChanged();
        }
        if (preset_store_->retranslateBuiltins()) {
            emit presetsChanged();
        }
    }
    return QObject::eventFilter(watched, event);
}

void ExportController::startRecoveryDrain() {
    if (busy()) {
        return;
    }
    completed_count_ = 0;
    failed_count_ = 0;
    current_count_ = 0;
    total_count_ = 0;
    cancellation_requested_ = false;
    errors_.clear();
    cancellation_token_ = std::make_shared<std::atomic_bool>(false);
    setStatusMessage(export_message(QT_TRANSLATE_NOOP(
        "ExportController",
        "Checking unfinished exports…"
    )));
    emit progressChanged();
    emit cancellationRequestedChanged();
    emit errorsChanged();
    const auto generation = ++export_generation_;
    const auto report_progress = [this, generation](
                                     const int completed,
                                     const int failed,
                                     const int current,
                                     const int total,
                                     const QString& current_title
                                 ) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, completed, failed, current, total, current_title] {
                applyProgress(
                    generation,
                    completed,
                    failed,
                    current,
                    total,
                    current_title
                );
            },
            Qt::QueuedConnection
        );
    };
    watcher_.setFuture(QtConcurrent::run(
        ExportTaskRunner::runDurableExport,
        export_backend_,
        QVector<BackendDurableExportTarget>{},
        QString{},
        true,
        cancellation_token_,
        report_progress
    ));
    emit busyChanged();
}

void ExportController::startExport(
    const QVariantList& targets,
    const QUrl& destination_folder,
    const QVariantMap& options
) {
    if (busy()) {
        return;
    }
    const QString folder_path = destination_folder.toLocalFile();
    if (folder_path.isEmpty() || !QDir(folder_path).exists()) {
        setStatusMessage(export_message(QT_TRANSLATE_NOOP(
            "ExportController",
            "Choose an export folder"
        )));
        return;
    }
    QVector<ExportPhoto> photos;
    photos.reserve(targets.size());
    QSet<QString> seen;
    for (const auto& value : targets) {
        const QVariantMap target = value.toMap();
        const QString photo_id = target.value(QStringLiteral("photoId")).toString();
        const QString source_path =
            target.value(QStringLiteral("sourcePath")).toString();
        if (photo_id.isEmpty() || source_path.isEmpty() || seen.contains(photo_id)) {
            continue;
        }
        seen.insert(photo_id);
        photos.push_back({
            .photo_id = photo_id,
            .source_path = source_path,
            .title = target.value(QStringLiteral("title")).toString(),
        });
    }
    if (photos.isEmpty()) {
        setStatusMessage(export_message(QT_TRANSLATE_NOOP(
            "ExportController",
            "Select at least one photo to export"
        )));
        return;
    }
    BackendExportOptions export_options;
    try {
        export_options = ExportSettingsCodec::fromVariantMap(options);
    } catch (const std::exception& error) {
        setRawStatusText(QString::fromUtf8(error.what()));
        return;
    }
    const QString extension = export_options.format == QStringLiteral("png")
        ? QStringLiteral("png")
        : export_options.format == QStringLiteral("tiff")
            ? QStringLiteral("tif")
            : QStringLiteral("jpg");
    const QString suffix = export_options.filename_suffix;
    const QDir folder(QDir(folder_path).absolutePath());
    QSet<QString> reserved_destinations;
    QVector<BackendDurableExportTarget> durable_targets;
    durable_targets.reserve(photos.size());
    for (const auto& photo : photos) {
        durable_targets.push_back({
            .photo_id = photo.photo_id,
            .source_path = photo.source_path,
            .output_path = unique_destination(
                folder,
                safe_stem(photo.title, photo.source_path),
                suffix,
                extension,
                reserved_destinations
            ),
        });
    }
    const QString settings_json =
        ExportSettingsCodec::toDurableJson(export_options);
    completed_count_ = 0;
    failed_count_ = 0;
    current_count_ = 0;
    total_count_ = checked_export_count(photos.size());
    cancellation_requested_ = false;
    errors_.clear();
    cancellation_token_ = std::make_shared<std::atomic_bool>(false);
    setStatusMessage(export_message(
        QT_TRANSLATE_NOOP("ExportController", "Queueing %1 photos…"),
        {total_count_}
    ));
    emit progressChanged();
    emit cancellationRequestedChanged();
    emit errorsChanged();
    const auto generation = ++export_generation_;
    // The controller waits for its future in the destructor, so queued updates
    // cannot outlive this receiver. A generation still protects a new export
    // from late updates posted by a previous run.
    const auto report_progress = [this, generation](
                                     const int completed,
                                     const int failed,
                                     const int current,
                                     const int total,
                                     const QString& current_title
                                 ) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, completed, failed, current, total, current_title] {
                applyProgress(
                    generation,
                    completed,
                    failed,
                    current,
                    total,
                    current_title
                );
            },
            Qt::QueuedConnection
        );
    };
    watcher_.setFuture(QtConcurrent::run(
        ExportTaskRunner::runDurableExport,
        export_backend_,
        durable_targets,
        settings_json,
        false,
        cancellation_token_,
        report_progress
    ));
    emit busyChanged();
}

void ExportController::cancelExport() {
    if (!busy() || cancellation_requested_ || !cancellation_token_) {
        return;
    }
    cancellation_requested_ = true;
    cancellation_token_->store(true, std::memory_order_relaxed);
    setStatusMessage(export_message(QT_TRANSLATE_NOOP(
        "ExportController",
        "Cancelling after the current photo…"
    )));
    emit cancellationRequestedChanged();
}

QString ExportController::savePreset(
    const QString& name,
    const QVariantMap& options
) {
    const QString id = preset_store_->save(name, options);
    if (!id.isEmpty()) {
        emit presetsChanged();
    }
    return id;
}

QString ExportController::updatePreset(
    const QString& preset_id,
    const QString& name,
    const QVariantMap& options
) {
    const QString id = preset_store_->update(preset_id, name, options);
    if (!id.isEmpty()) {
        emit presetsChanged();
    }
    return id;
}

void ExportController::removePreset(const QString& preset_id) {
    if (preset_store_->remove(preset_id)) {
        emit presetsChanged();
    }
}

QString ExportController::saveWatermark(
    const QString& name,
    const QVariantMap& definition
) {
    const QString id = watermark_store_->save(name, definition);
    if (!id.isEmpty()) {
        emit watermarksChanged();
    }
    return id;
}

QString ExportController::updateWatermark(
    const QString& watermark_id,
    const QString& name,
    const QVariantMap& definition
) {
    const QString id = watermark_store_->update(
        watermark_id,
        name,
        definition
    );
    if (!id.isEmpty()) {
        emit watermarksChanged();
    }
    return id;
}

void ExportController::removeWatermark(const QString& watermark_id) {
    if (watermark_store_->remove(watermark_id)) {
        emit watermarksChanged();
    }
}

void ExportController::finishExport() {
    const ExportTaskResult result = watcher_.result();
    completed_count_ = result.completed;
    failed_count_ = result.failed;
    current_count_ = std::min(
        result.requested,
        result.completed + result.failed + result.cancelled_items
    );
    total_count_ = result.requested;
    errors_ = result.errors;
    const bool was_cancelling = cancellation_requested_;
    cancellation_requested_ = false;
    cancellation_token_.reset();
    if (result.recovered_on_startup && result.requested == 0) {
        setStatusMessage(export_message(QT_TRANSLATE_NOOP(
            "ExportController",
            "No unfinished exports"
        )));
    } else if (result.recovered_on_startup && result.cancelled) {
        setStatusMessage(export_message(QT_TRANSLATE_NOOP(
            "ExportController",
            "Resuming exports paused"
        )));
    } else if (result.recovered_on_startup && result.paused_conflicts > 0) {
        setStatusMessage(export_message(
            QT_TRANSLATE_NOOP(
                "ExportController",
                "Resumed %1 photos · %2 need attention"
            ),
            {result.completed, result.paused_conflicts}
        ));
    } else if (result.recovered_on_startup && result.failed == 0) {
        setStatusMessage(export_message(
            QT_TRANSLATE_NOOP(
                "ExportController",
                "Resumed %1 exports"
            ),
            {result.completed}
        ));
    } else if (result.recovered_on_startup) {
        setStatusMessage(export_message(
            QT_TRANSLATE_NOOP(
                "ExportController",
                "Resumed %1 exports · %2 failed"
            ),
            {result.completed, result.failed}
        ));
    } else if (result.cancelled) {
        setStatusMessage(export_message(
            QT_TRANSLATE_NOOP(
                "ExportController",
                "Export cancelled · %1 exported · %2 failed"
            ),
            {result.completed, result.failed}
        ));
    } else if (result.paused_conflicts > 0) {
        setStatusMessage(export_message(
            QT_TRANSLATE_NOOP(
                "ExportController",
                "Exported %1 photos · %2 need attention"
            ),
            {result.completed, result.paused_conflicts}
        ));
    } else if (result.failed == 0) {
        setStatusMessage(export_message(
            QT_TRANSLATE_NOOP(
                "ExportController",
                "Exported %1 photos"
            ),
            {result.completed}
        ));
    } else {
        setStatusMessage(export_message(
            QT_TRANSLATE_NOOP(
                "ExportController",
                "Exported %1 photos · %2 failed"
            ),
            {result.completed, result.failed}
        ));
    }
    emit progressChanged();
    if (was_cancelling) {
        emit cancellationRequestedChanged();
    }
    emit errorsChanged();
    emit busyChanged();
    if (!result.recovered_on_startup) {
        emit exportFinished(
            result.completed,
            result.failed,
            result.cancelled,
            result.destination_paths,
            result.errors
        );
    }
}

void ExportController::applyProgress(
    const quint64 generation,
    const int completed,
    const int failed,
    const int current,
    const int total,
    const QString& current_title
) {
    if (!busy() || generation != export_generation_) {
        return;
    }
    completed_count_ = completed;
    failed_count_ = failed;
    current_count_ = current;
    total_count_ = total;
    if (!cancellation_requested_) {
        setStatusMessage(export_message(
            QT_TRANSLATE_NOOP(
                "ExportController",
                "Exporting %1 of %2 · %3"
            ),
            {current, total, current_title}
        ));
    }
    emit progressChanged();
}

void ExportController::setStatusMessage(LocalizedUiMessage status) {
    if (raw_status_text_.isEmpty() && status_message_ == status) {
        return;
    }
    raw_status_text_.clear();
    status_message_ = std::move(status);
    emit statusTextChanged();
}

void ExportController::setRawStatusText(QString status) {
    if (status_message_.isEmpty() && raw_status_text_ == status) {
        return;
    }
    status_message_.clear();
    raw_status_text_ = std::move(status);
    emit statusTextChanged();
}
