#include "review_import_coordinator.hpp"

#include <QtConcurrentRun>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

constexpr int SCAN_PROGRESS_POLL_MS = 150;
constexpr quint64 STREAM_REFRESH_STRIDE = 16;
constexpr qint64 STREAM_REFRESH_MIN_INTERVAL_MS = 400;
constexpr qint64 STREAM_VISUAL_REFRESH_MIN_INTERVAL_MS = 150;

[[nodiscard]] LocalizedUiMessage import_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

[[nodiscard]] QString phase_name(const BackendScanPhase phase) {
    switch (phase) {
    case BackendScanPhase::Discovering:
        return QStringLiteral("discovering");
    case BackendScanPhase::PreparingPreviews:
        return QStringLiteral("preparing-previews");
    case BackendScanPhase::Cancelling:
        return QStringLiteral("cancelling");
    case BackendScanPhase::Completed:
        return QStringLiteral("completed");
    case BackendScanPhase::Cancelled:
        return QStringLiteral("cancelled");
    case BackendScanPhase::Failed:
        return QStringLiteral("failed");
    case BackendScanPhase::Idle:
    default:
        return QStringLiteral("idle");
    }
}

} // namespace

ReviewImportCoordinator::ReviewImportCoordinator(
    Operations operations,
    QObject* parent
)
    : QObject(parent),
      operations_(std::move(operations)) {
    if (!operations_.begin || !operations_.scan || !operations_.progress
        || !operations_.cancel) {
        throw std::invalid_argument(
            "all Review import operations are required"
        );
    }
    progress_timer_.setInterval(SCAN_PROGRESS_POLL_MS);
    progress_timer_.setTimerType(Qt::CoarseTimer);
    connect(
        &progress_timer_,
        &QTimer::timeout,
        this,
        &ReviewImportCoordinator::pollProgress
    );
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewImportCoordinator::finishTask
    );
}

ReviewImportCoordinator::~ReviewImportCoordinator() {
    progress_timer_.stop();
    if (running_) {
        try {
            static_cast<void>(operations_.cancel(generation_));
        } catch (const std::exception&) {
        }
    }
    watcher_.waitForFinished();
}

bool ReviewImportCoordinator::scanning() const noexcept {
    return running_;
}

QString ReviewImportCoordinator::folderPath() const {
    return folder_path_;
}

QVariantMap ReviewImportCoordinator::progress() const {
    return {
        {QStringLiteral("scanId"), QVariant::fromValue(generation_)},
        {QStringLiteral("updateSequence"), QVariant::fromValue(update_sequence_)},
        {QStringLiteral("phase"), phase_name(phase_)},
        {QStringLiteral("filesSeen"), QVariant::fromValue(files_seen_)},
        {QStringLiteral("supportedFiles"), QVariant::fromValue(supported_files_)},
        {QStringLiteral("cataloguedFiles"), QVariant::fromValue(cataloguedFiles())},
        {QStringLiteral("insertedFiles"), QVariant::fromValue(inserted_files_)},
        {QStringLiteral("unchangedFiles"), QVariant::fromValue(unchanged_files_)},
        {
            QStringLiteral("revalidationFiles"),
            QVariant::fromValue(revalidation_files_),
        },
        {QStringLiteral("decodeQueued"), QVariant::fromValue(decode_queued_)},
        {
            QStringLiteral("previewArtifactsReady"),
            QVariant::fromValue(preview_artifacts_ready_),
        },
        {QStringLiteral("decodeCompleted"), QVariant::fromValue(decode_completed_)},
        {
            QStringLiteral("decodeHardFailures"),
            QVariant::fromValue(decode_hard_failures_),
        },
        {QStringLiteral("previewFailures"), QVariant::fromValue(preview_failures_)},
        {QStringLiteral("decodeCancelled"), QVariant::fromValue(decode_cancelled_)},
        {QStringLiteral("skippedFiles"), QVariant::fromValue(skipped_files_)},
        {QStringLiteral("issueCount"), QVariant::fromValue(issue_count_)},
        {QStringLiteral("targetPath"), folder_path_},
        {QStringLiteral("cancellable"), running_},
    };
}

LocalizedUiMessage ReviewImportCoordinator::statusMessage() const {
    return status_message_;
}

LocalizedUiMessage ReviewImportCoordinator::refreshingStatusMessage() const {
    if (!terminal_error_.isEmpty()) {
        return import_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Refreshing photos retained before import stopped…"
        ));
    }
    if (terminal_cancelled_) {
        return import_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Refreshing photos retained before import was cancelled…"
        ));
    }
    return import_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Loading the local Library…"
    ));
}

LocalizedUiMessage ReviewImportCoordinator::readyStatusMessage(
    const quint64 total_items,
    const int visible_items,
    const bool loading_more
) const {
    if (!terminal_error_.isEmpty()) {
        return import_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Import stopped · %1 photos remain available · filesystem/import "
                "error: %2 · %3 decode failures · %4 preview failures"
            ),
            {
                total_items,
                terminal_error_,
                decode_hard_failures_,
                preview_failures_,
            }
        );
    }
    if (terminal_cancelled_) {
        return import_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Import cancelled · %1 photos remain available · %2 filesystem "
                "issues · %3 decode failures · %4 preview failures · %5 decode "
                "jobs cancelled"
            ),
            {
                total_items,
                issue_count_,
                decode_hard_failures_,
                preview_failures_,
                decode_cancelled_,
            }
        );
    }
    if (total_items == 0) {
        return import_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "Local Library is empty · add a photo folder to begin"
        ));
    }
    const char* const source = loading_more
        ? QT_TRANSLATE_NOOP(
              "ReviewController",
              "%1 / %2 loaded · %3 supported · %4/%5 preview checks completed "
              "· %6 filesystem issues · %7 decode failures · %8 preview "
              "failures · loading more"
          )
        : QT_TRANSLATE_NOOP(
              "ReviewController",
              "%1 / %2 loaded · %3 supported · %4/%5 preview checks completed "
              "· %6 filesystem issues · %7 decode failures · %8 preview failures"
          );
    return import_message(
        source,
        {
            visible_items,
            total_items,
            supported_files_,
            decode_completed_,
            decode_queued_,
            issue_count_,
            decode_hard_failures_,
            preview_failures_,
        }
    );
}

bool ReviewImportCoordinator::start(
    const QUrl& folder_url,
    const bool admitted
) {
    if (!admitted || running_) {
        return false;
    }
    const QString path = folder_url.toLocalFile();
    if (path.isEmpty()) {
        publishStatus(import_message(QT_TRANSLATE_NOOP(
            "ReviewController",
            "The selected folder is not a local path"
        )));
        return false;
    }

    quint64 next_generation = generation_ + 1;
    if (next_generation == 0) {
        ++next_generation;
    }
    try {
        operations_.begin(next_generation);
    } catch (const std::exception& error) {
        publishStatus(import_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not start import · %1"
            ),
            {QString::fromUtf8(error.what())}
        ));
        return false;
    }

    generation_ = next_generation;
    folder_path_ = path;
    resetProgress();
    phase_ = BackendScanPhase::Discovering;
    terminal_error_.clear();
    terminal_cancelled_ = false;
    running_ = true;
    clock_.restart();
    progress_timer_.start();
    updateActiveStatus();
    emit folderPathChanged();
    emit runningChanged();
    emit progressChanged();
    emit statusMessageChanged();
    watcher_.setFuture(QtConcurrent::run(
        runTask,
        operations_,
        folder_path_,
        generation_
    ));
    return true;
}

bool ReviewImportCoordinator::cancel() {
    if (!running_ || phase_ == BackendScanPhase::Cancelling) {
        return false;
    }
    try {
        if (!operations_.cancel(generation_)) {
            return false;
        }
    } catch (const std::exception& error) {
        publishStatus(import_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not cancel import · %1"
            ),
            {QString::fromUtf8(error.what())}
        ));
        return false;
    }
    phase_ = BackendScanPhase::Cancelling;
    updateActiveStatus();
    emit progressChanged();
    emit statusMessageChanged();
    return true;
}

bool ReviewImportCoordinator::takeStreamRefreshRequest(
    const int visible_items,
    const bool page_running
) {
    if (!running_ || page_running) {
        return false;
    }
    const quint64 catalogued = cataloguedFiles();
    const qint64 elapsed = clock_.isValid() ? clock_.elapsed() : 0;
    const bool first_visible_page = visible_items == 0 && catalogued > 0
        && last_stream_refresh_ms_ < 0;
    const bool paced_refresh = catalogued >= next_stream_refresh_at_
        && (last_stream_refresh_ms_ < 0
            || elapsed - last_stream_refresh_ms_
                >= STREAM_REFRESH_MIN_INTERVAL_MS);
    const bool visual_refresh =
        preview_artifacts_ready_ > last_stream_visual_refresh_at_
        && (last_stream_refresh_ms_ < 0
            || elapsed - last_stream_refresh_ms_
                >= STREAM_VISUAL_REFRESH_MIN_INTERVAL_MS);
    if (!first_visible_page && !paced_refresh && !visual_refresh) {
        return false;
    }
    last_stream_refresh_ms_ = elapsed;
    next_stream_refresh_at_ = catalogued + STREAM_REFRESH_STRIDE;
    last_stream_visual_refresh_at_ = preview_artifacts_ready_;
    return true;
}

void ReviewImportCoordinator::retranslateUi() {
    if (!status_message_.isEmpty()) {
        emit statusMessageChanged();
    }
}

ReviewImportCoordinator::TaskResult ReviewImportCoordinator::runTask(
    Operations operations,
    QString folder_path,
    const quint64 generation
) {
    TaskResult result;
    result.generation = generation;
    try {
        result.report = operations.scan(folder_path, generation);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

quint64 ReviewImportCoordinator::cataloguedFiles() const noexcept {
    return inserted_files_ + unchanged_files_ + revalidation_files_;
}

void ReviewImportCoordinator::pollProgress() {
    if (generation_ == 0) {
        return;
    }
    BackendScanProgress progress;
    try {
        progress = operations_.progress(generation_);
    } catch (const std::exception& error) {
        if (running_) {
            publishStatus(import_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Import progress unavailable · %1"
                ),
                {QString::fromUtf8(error.what())}
            ));
        }
        return;
    }
    if (!progress.valid || progress.scan_id != generation_
        || progress.update_sequence <= update_sequence_) {
        return;
    }
    applyProgress(progress);
    if (running_) {
        updateActiveStatus();
    }
    emit progressChanged();
    if (running_) {
        emit statusMessageChanged();
    }
}

void ReviewImportCoordinator::finishTask() {
    const TaskResult result = watcher_.result();
    if (result.generation != generation_) {
        return;
    }
    progress_timer_.stop();
    pollProgress();
    const QString previous_folder_path = folder_path_;
    if (!result.error.isEmpty()) {
        phase_ = BackendScanPhase::Failed;
        terminal_error_ = result.error;
        terminal_cancelled_ = false;
    } else {
        folder_path_ = result.report.folder_path;
        files_seen_ = result.report.files_seen;
        supported_files_ = result.report.supported_files;
        inserted_files_ = result.report.inserted;
        unchanged_files_ = result.report.unchanged;
        revalidation_files_ = result.report.needs_revalidation;
        decode_queued_ = result.report.decode_queued;
        decode_completed_ = result.report.decode_completed;
        decode_hard_failures_ = result.report.decode_hard_failures;
        preview_failures_ = result.report.preview_failures;
        decode_cancelled_ = result.report.decode_cancelled;
        issue_count_ = result.report.issue_count;
        terminal_cancelled_ = result.report.cancelled;
        phase_ = result.report.cancelled ? BackendScanPhase::Cancelled
                                         : BackendScanPhase::Completed;
    }
    running_ = false;
    emit terminalRefreshRequested();
    if (folder_path_ != previous_folder_path) {
        emit folderPathChanged();
    }
    emit runningChanged();
    emit progressChanged();
}

void ReviewImportCoordinator::publishStatus(LocalizedUiMessage status) {
    status_message_ = std::move(status);
    emit statusMessageChanged();
}

void ReviewImportCoordinator::updateActiveStatus() {
    const quint64 catalogued = cataloguedFiles();
    switch (phase_) {
    case BackendScanPhase::PreparingPreviews:
        status_message_ = import_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Import catalogued %1 files · finishing %2 queued preview "
                "checks · %3 filesystem issues"
            ),
            {catalogued, decode_queued_, issue_count_}
        );
        break;
    case BackendScanPhase::Cancelling:
        status_message_ = import_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Stopping import safely · %1 files retained · queued checks are "
                "being cancelled · current preview may finish"
            ),
            {catalogued}
        );
        break;
    case BackendScanPhase::Discovering:
    default:
        status_message_ = import_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Importing · %1 files checked · %2 supported · %3 catalogued · "
                "%4 preview checks queued · %5 filesystem issues"
            ),
            {
                files_seen_,
                supported_files_,
                catalogued,
                decode_queued_,
                issue_count_,
            }
        );
        break;
    }
}

void ReviewImportCoordinator::resetProgress() {
    update_sequence_ = 0;
    files_seen_ = 0;
    supported_files_ = 0;
    inserted_files_ = 0;
    unchanged_files_ = 0;
    revalidation_files_ = 0;
    decode_queued_ = 0;
    preview_artifacts_ready_ = 0;
    decode_completed_ = 0;
    decode_hard_failures_ = 0;
    preview_failures_ = 0;
    decode_cancelled_ = 0;
    skipped_files_ = 0;
    issue_count_ = 0;
    next_stream_refresh_at_ = 1;
    last_stream_visual_refresh_at_ = 0;
    last_stream_refresh_ms_ = -1;
}

void ReviewImportCoordinator::applyProgress(
    const BackendScanProgress& progress
) {
    update_sequence_ = progress.update_sequence;
    files_seen_ = progress.files_seen;
    supported_files_ = progress.supported_files;
    inserted_files_ = progress.inserted;
    unchanged_files_ = progress.unchanged;
    revalidation_files_ = progress.needs_revalidation;
    decode_queued_ = progress.decode_queued;
    preview_artifacts_ready_ = progress.preview_artifacts_ready;
    decode_completed_ = progress.decode_completed;
    decode_hard_failures_ = progress.decode_hard_failures;
    preview_failures_ = progress.preview_failures;
    decode_cancelled_ = progress.decode_cancelled;
    skipped_files_ = progress.skipped;
    issue_count_ = progress.issue_count;
    phase_ = progress.phase;
}
