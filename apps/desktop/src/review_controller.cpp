#include "review_controller.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QSet>
#include <QSettings>
#include <QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t REVIEW_PAGE_SIZE = 96;
constexpr int SCAN_PROGRESS_POLL_MS = 150;
constexpr quint64 STREAM_REFRESH_STRIDE = 16;
constexpr qint64 STREAM_REFRESH_MIN_INTERVAL_MS = 400;
constexpr qint64 STREAM_VISUAL_REFRESH_MIN_INTERVAL_MS = 150;
constexpr auto color_labels_settings_key = "review/color_labels";

[[nodiscard]] LocalizedUiMessage review_message(
    const char *const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}) {
  return {"ReviewController", source, arguments};
}

[[nodiscard]] ScanTaskResult run_scan(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& folder_path,
    const quint64 generation
) {
    ScanTaskResult result;
    result.generation = generation;
    try {
        result.report = backend->scanFolder(folder_path, generation);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] PageTaskResult run_page(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& cursor_path,
    const QString& cursor_representation_id,
    const quint64 library_generation,
    const quint64 request_id,
    const PageTaskKind kind,
    const QVector<QString>& required_representation_ids
) {
    PageTaskResult result;
    result.library_generation = library_generation;
    result.request_id = request_id;
    result.kind = kind;
    try {
        if (kind != PageTaskKind::FinalReset) {
            result.page = backend->reviewPage(
                cursor_path,
                cursor_representation_id,
                REVIEW_PAGE_SIZE
            );
            return result;
        }

        QSet<QString> required_keys;
        required_keys.reserve(required_representation_ids.size());
        for (const auto& representation_id : required_representation_ids) {
            required_keys.insert(representation_id);
        }
        QSet<QString> seen_keys;
        QString next_path;
        QString next_representation_id;
        bool first_page = true;
        while (true) {
            BackendReviewPage page = backend->reviewPage(
                next_path,
                next_representation_id,
                REVIEW_PAGE_SIZE
            );
            const bool page_is_empty = page.items.isEmpty();
            if (!first_page && page.total_items != result.page.total_items) {
                throw std::runtime_error(
                    "Catalog changed while rebuilding the stable Library prefix"
                );
            }
            if (first_page) {
                result.page.total_items = page.total_items;
                first_page = false;
            }
            for (auto& item : page.items) {
                if (item.representation_id.isEmpty()
                    || seen_keys.contains(item.representation_id)) {
                    throw std::runtime_error(
                        "Library refresh returned an invalid or duplicate stable key"
                    );
                }
                seen_keys.insert(item.representation_id);
                required_keys.remove(item.representation_id);
                result.page.items.push_back(std::move(item));
            }
            result.page.has_more = page.has_more;
            result.page.next_cursor_path = std::move(page.next_cursor_path);
            result.page.next_cursor_representation_id =
                std::move(page.next_cursor_representation_id);
            if (result.page.has_more
                && (page_is_empty
                || result.page.next_cursor_path.isEmpty()
                || result.page.next_cursor_representation_id.isEmpty()
                || (result.page.next_cursor_path == next_path
                    && result.page.next_cursor_representation_id
                        == next_representation_id))) {
                throw std::runtime_error(
                    "Library refresh did not advance its stable pagination cursor"
                );
            }
            if (required_keys.isEmpty() || !result.page.has_more) {
                break;
            }
            next_path = result.page.next_cursor_path;
            next_representation_id = result.page.next_cursor_representation_id;
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] QString decision_flag_name(const BackendReviewDecisionFlag flag) {
    switch (flag) {
    case BackendReviewDecisionFlag::Unflagged:
        return QStringLiteral("unflagged");
    case BackendReviewDecisionFlag::Picked:
        return QStringLiteral("picked");
    case BackendReviewDecisionFlag::Rejected:
        return QStringLiteral("rejected");
    }
    throw std::invalid_argument("unknown Review decision flag");
}

[[nodiscard]] QVector<ReviewItem> review_items(QVector<BackendReviewItem> source) {
    QVector<ReviewItem> items;
    items.reserve(source.size());
    for (auto& item : source) {
        items.push_back({
            .photo_id = std::move(item.photo_id),
            .representation_id = std::move(item.representation_id),
            .visual_handle = std::move(item.visual_handle),
            .decision_head_sequence = item.decision_head_sequence,
            .decision_flag = decision_flag_name(item.decision_flag),
            .decision_rating = static_cast<int>(item.decision_rating),
            .has_development_edits = item.has_development_edits,
            .title = std::move(item.title),
            .source_path = std::move(item.source_path),
            .visual_role = std::move(item.visual_role),
            .visual_width = item.visual_width,
            .visual_height = item.visual_height,
            .has_visual = item.has_visual,
            .has_metadata = item.has_metadata,
            .camera_make = std::move(item.camera_make),
            .camera_model = std::move(item.camera_model),
            .lens_make = std::move(item.lens_make),
            .lens_model = std::move(item.lens_model),
            .captured_at_unix_seconds = item.captured_at_unix_seconds,
            .iso_speed = item.iso_speed,
            .exposure_time_seconds = item.exposure_time_seconds,
            .aperture_f_number = item.aperture_f_number,
            .focal_length_mm = item.focal_length_mm,
            .focal_length_35mm = item.focal_length_35mm,
            .raw_width = item.raw_width,
            .raw_height = item.raw_height,
            .sensor_bits = item.sensor_bits,
            .cfa_pattern = std::move(item.cfa_pattern),
            .dng_version = std::move(item.dng_version),
            .has_technical_observation = item.has_technical_observation,
            .technical_input_width = item.technical_input_width,
            .technical_input_height = item.technical_input_height,
            .technical_preprocessing_version = std::move(
                item.technical_preprocessing_version
            ),
            .technical_implementation_version = std::move(
                item.technical_implementation_version
            ),
            .mean_luma = item.mean_luma,
            .p01_luma = item.p01_luma,
            .p50_luma = item.p50_luma,
            .p99_luma = item.p99_luma,
            .near_black_fraction = item.near_black_fraction,
            .near_white_fraction = item.near_white_fraction,
            .laplacian_variance = item.laplacian_variance,
            .edge_energy = item.edge_energy,
        });
    }
    return items;
}

[[nodiscard]] int bounded_count(const quint64 count) {
    return static_cast<int>(std::min<quint64>(
        count,
        static_cast<quint64>(std::numeric_limits<int>::max())
    ));
}

[[nodiscard]] std::optional<BackendPairwiseOutcome> pairwise_outcome(
    const int outcome
) {
    switch (outcome) {
    case 0:
        return BackendPairwiseOutcome::LeftPreferred;
    case 1:
        return BackendPairwiseOutcome::RightPreferred;
    case 2:
        return BackendPairwiseOutcome::KeepBoth;
    case 3:
        return BackendPairwiseOutcome::KeepNeither;
    case 4:
        return BackendPairwiseOutcome::CannotCompare;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] ReviewEvidenceTaskResult record_comparison(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& presentation_id,
    const BackendPairwiseOutcome outcome
) {
    ReviewEvidenceTaskResult result;
    result.kind = ReviewEvidenceTaskKind::Record;
    try {
        result.feedback = backend->recordReviewComparison(
            presentation_id,
            outcome
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] ReviewEvidenceTaskResult forget_comparison(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& event_id
) {
    ReviewEvidenceTaskResult result;
    result.kind = ReviewEvidenceTaskKind::Forget;
    try {
        result.forget = backend->forgetReviewFeedback(event_id);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] std::optional<BackendReviewDecisionFlag> decision_flag(
    const QString& flag
) {
    if (flag == QStringLiteral("unflagged")) {
        return BackendReviewDecisionFlag::Unflagged;
    }
    if (flag == QStringLiteral("picked")) {
        return BackendReviewDecisionFlag::Picked;
    }
    if (flag == QStringLiteral("rejected")) {
        return BackendReviewDecisionFlag::Rejected;
    }
    return std::nullopt;
}

[[nodiscard]] BackendReviewDecisionState backend_decision_state(
    const QString& photo_id,
    const ReviewDecisionValue& value
) {
    const auto flag = decision_flag(value.flag);
    if (!flag || value.rating < 0 || value.rating > 5) {
        throw std::invalid_argument("Review model contains an invalid decision state");
    }
    return {
        .photo_id = photo_id,
        .head_sequence = value.head_sequence,
        .flag = *flag,
        .rating = static_cast<std::uint8_t>(value.rating),
    };
}

[[nodiscard]] ReviewDecisionTaskResult run_decision_mutation(
    const std::shared_ptr<DesktopBackend>& backend,
    const ReviewDecisionMutationRequest& request
) {
    ReviewDecisionTaskResult result;
    result.is_undo = request.is_undo;
    try {
        result.receipt = backend->setReviewPhotoDecision(
            request.photo_id,
            request.expected_head_sequence,
            request.desired_flag,
            request.desired_rating
        );
        return result;
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    try {
        result.authoritative = backend->reviewPhotoDecisionState(request.photo_id);
        result.has_authoritative = true;
    } catch (const std::exception& error) {
        result.refresh_error = QString::fromUtf8(error.what());
    }
    return result;
}

} // namespace

ReviewController::ReviewController(
    std::shared_ptr<DesktopBackend> backend,
    const QString& isolated_settings_file,
    QObject* parent
)
    : QObject(parent),
      backend_(std::move(backend)),
      model_(this),
      filtered_model_(this),
      settings_(isolated_settings_file.isEmpty()
              ? std::make_unique<QSettings>()
              : std::make_unique<QSettings>(
                    isolated_settings_file,
                    QSettings::IniFormat
                )) {
    model_.restoreColorLabels(
        settings_->value(QString::fromLatin1(color_labels_settings_key)).toMap()
    );
    filtered_model_.setSourceModel(&model_);
    connect(
        &filtered_model_,
        &ReviewFilterModel::filtersChanged,
        this,
        &ReviewController::filtersChanged
    );
    const auto notify_filtered_count = [this]() { emit filtersChanged(); };
    connect(&filtered_model_, &QAbstractItemModel::modelReset,
            this, notify_filtered_count);
    connect(&filtered_model_, &QAbstractItemModel::rowsInserted,
            this, [notify_filtered_count](const QModelIndex&, const int, const int) {
                notify_filtered_count();
            });
    connect(&filtered_model_, &QAbstractItemModel::rowsRemoved,
            this, [notify_filtered_count](const QModelIndex&, const int, const int) {
                notify_filtered_count();
            });
    model_.replace({}, library_generation_);
    scan_progress_timer_.setInterval(SCAN_PROGRESS_POLL_MS);
    scan_progress_timer_.setTimerType(Qt::CoarseTimer);
    connect(
        &scan_watcher_,
        &QFutureWatcher<ScanTaskResult>::finished,
        this,
        &ReviewController::finishScan
    );
    connect(
        &page_watcher_,
        &QFutureWatcher<PageTaskResult>::finished,
        this,
        &ReviewController::finishPage
    );
    connect(
        &evidence_watcher_,
        &QFutureWatcher<ReviewEvidenceTaskResult>::finished,
        this,
        &ReviewController::finishEvidenceTask
    );
    connect(
        &decision_watcher_,
        &QFutureWatcher<ReviewDecisionTaskResult>::finished,
        this,
        &ReviewController::finishDecisionTask
    );
    connect(
        &scan_progress_timer_,
        &QTimer::timeout,
        this,
        &ReviewController::pollScanProgress
    );
  if (auto *const application = QCoreApplication::instance()) {
    application->installEventFilter(this);
  }
    QTimer::singleShot(0, this, [this]() {
        refreshSharedGradeNodes();
        startPage(
            scan_running_ ? PageTaskKind::StreamingPrefix
                          : PageTaskKind::InitialReset
        );
    });
}

ReviewController::~ReviewController() {
    scan_progress_timer_.stop();
    if (scan_running_) {
        try {
            static_cast<void>(backend_->cancelFolderScan(scan_generation_));
        } catch (const std::exception&) {
        }
    }
    scan_watcher_.waitForFinished();
    page_watcher_.waitForFinished();
    evidence_watcher_.waitForFinished();
    decision_watcher_.waitForFinished();
}

bool ReviewController::busy() const noexcept {
    return model_.rowCount() == 0
        && (scan_running_ || page_running_ || terminal_refresh_active_);
}

bool ReviewController::scanning() const noexcept {
    return scan_running_;
}

bool ReviewController::refreshing() const noexcept {
    return page_reset_running_ || terminal_refresh_active_;
}

bool ReviewController::loadingMore() const noexcept {
    return page_running_ && !page_reset_running_ && model_.rowCount() > 0;
}

bool ReviewController::hasMore() const noexcept {
    return has_more_;
}

QString ReviewController::folderPath() const {
    return folder_path_;
}

QString ReviewController::statusText() const {
    return status_message_.translated();
}

QVariantMap ReviewController::scanProgress() const {
    const quint64 catalogued = inserted_files_ + unchanged_files_ + revalidation_files_;
    QString phase = QStringLiteral("idle");
    switch (scan_phase_) {
    case BackendScanPhase::Idle:
        break;
    case BackendScanPhase::Discovering:
        phase = QStringLiteral("discovering");
        break;
    case BackendScanPhase::PreparingPreviews:
        phase = QStringLiteral("preparing-previews");
        break;
    case BackendScanPhase::Cancelling:
        phase = QStringLiteral("cancelling");
        break;
    case BackendScanPhase::Completed:
        phase = QStringLiteral("completed");
        break;
    case BackendScanPhase::Cancelled:
        phase = QStringLiteral("cancelled");
        break;
    case BackendScanPhase::Failed:
        phase = QStringLiteral("failed");
        break;
    }
    return {
        {QStringLiteral("scanId"), QVariant::fromValue(scan_generation_)},
        {QStringLiteral("updateSequence"), QVariant::fromValue(scan_update_sequence_)},
        {QStringLiteral("phase"), phase},
        {QStringLiteral("filesSeen"), QVariant::fromValue(files_seen_)},
        {QStringLiteral("supportedFiles"), QVariant::fromValue(supported_files_)},
        {QStringLiteral("cataloguedFiles"), QVariant::fromValue(catalogued)},
        {QStringLiteral("insertedFiles"), QVariant::fromValue(inserted_files_)},
        {QStringLiteral("unchangedFiles"), QVariant::fromValue(unchanged_files_)},
        {QStringLiteral("revalidationFiles"), QVariant::fromValue(revalidation_files_)},
        {QStringLiteral("decodeQueued"), QVariant::fromValue(decode_queued_)},
        {QStringLiteral("previewArtifactsReady"), QVariant::fromValue(preview_artifacts_ready_)},
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
        {QStringLiteral("cancellable"), scan_running_},
    };
}

int ReviewController::itemCount() const {
    return bounded_count(total_items_);
}

bool ReviewController::comparisonBusy() const noexcept {
    return evidence_session_.busy();
}

bool ReviewController::canUndoComparison() const noexcept {
    return evidence_session_.canForget();
}

int ReviewController::sessionEvidenceCount() const noexcept {
    return evidence_session_.activeCount();
}

QString ReviewController::comparisonStatusText() const {
    return comparison_status_message_.translated();
}

bool ReviewController::decisionBusy() const noexcept {
    return decision_session_.busy();
}

bool ReviewController::canUndoDecision() const {
    return decision_session_.canUndo();
}

QString ReviewController::decisionStatusText() const {
    return decision_status_message_.translated();
}

QString ReviewController::filterFlag() const {
    return filtered_model_.flagFilter();
}

int ReviewController::filterMinimumRating() const noexcept {
    return filtered_model_.minimumRating();
}

QString ReviewController::filterColorLabel() const {
    return filtered_model_.colorFilter();
}

QString ReviewController::filterEditState() const {
    return filtered_model_.editFilter();
}

int ReviewController::filteredItemCount() const noexcept {
    return filtered_model_.rowCount();
}

QVariantList ReviewController::sharedGradeNodes() const {
    QVariantList result;
    result.reserve(shared_grade_nodes_.size());
    for (const auto& shared : shared_grade_nodes_) {
        result.push_back(QVariantMap{
            {QStringLiteral("layerId"), shared.layer_id},
            {QStringLiteral("revisionId"), shared.revision_id},
            {QStringLiteral("revisionNumber"), shared.revision_number},
            {QStringLiteral("label"), shared.label},
        });
    }
    return result;
}

QAbstractItemModel* ReviewController::model() noexcept {
    return &filtered_model_;
}

ReviewModel* ReviewController::reviewModel() noexcept {
    return &model_;
}

void ReviewController::scanFolder(const QUrl& folder_url) {
    if (scan_running_ || page_running_ || terminal_refresh_active_
        || evidence_session_.busy() || decision_session_.busy()) {
        return;
    }
    const QString path = folder_url.toLocalFile();
    if (path.isEmpty()) {
    setStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "The selected folder is not a local path")));
        return;
    }

    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    const bool old_refreshing = refreshing();
    ++scan_generation_;
    if (scan_generation_ == 0) {
        ++scan_generation_;
    }
    try {
        backend_->beginFolderScan(scan_generation_);
    } catch (const std::exception& error) {
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController", "Could not start import · %1"),
        {QString::fromUtf8(error.what())})
        );
        return;
    }
    scan_update_sequence_ = 0;
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
    scan_phase_ = BackendScanPhase::Discovering;
    scan_terminal_error_.clear();
    scan_terminal_cancelled_ = false;
    final_page_refresh_pending_ = false;
    terminal_refresh_active_ = false;
    setHasMore(false);
    folder_path_ = path;
    emit folderPathChanged();
    scan_running_ = true;
    scan_clock_.restart();
    scan_progress_timer_.start();
    emit scanningChanged();
    emit scanProgressChanged();
    emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
    updateScanStatus();
    scan_watcher_.setFuture(QtConcurrent::run(
        [backend = backend_, folder = folder_path_, generation = scan_generation_]() {
            return run_scan(backend, folder, generation);
        }
    ));
}

void ReviewController::cancelScan() {
    if (!scan_running_ || scan_phase_ == BackendScanPhase::Cancelling) {
        return;
    }
    try {
        if (!backend_->cancelFolderScan(scan_generation_)) {
            return;
        }
    } catch (const std::exception& error) {
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController", "Could not cancel import · %1"),
        {QString::fromUtf8(error.what())})
        );
        return;
    }
    scan_phase_ = BackendScanPhase::Cancelling;
    emit scanProgressChanged();
    updateScanStatus();
}

void ReviewController::loadMore() {
    if (!has_more_ || scan_running_ || refreshing() || page_running_
        || evidence_session_.busy() || decision_session_.busy()) {
        return;
    }
    startPage(PageTaskKind::Append);
}

QVariantMap ReviewController::prepareComparison(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) {
    if (evidence_session_.busy() || decision_session_.busy() || scan_running_
        || refreshing() || page_running_) {
        return {};
    }
    if (left_visual_handle.trimmed().isEmpty()
        || right_visual_handle.trimmed().isEmpty()) {
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Choose two verified visuals before comparing")));
        return {};
    }
    try {
        const auto presentation = backend_->prepareReviewComparison(
            left_visual_handle,
            right_visual_handle
        );
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Loading two exact Compare frames with durable provenance…"))
        );
        return {
            {QStringLiteral("presentationId"), presentation.presentation_id},
            {QStringLiteral("leftRequestTicket"), presentation.left_request_ticket},
            {QStringLiteral("rightRequestTicket"), presentation.right_request_ticket},
            {
                QStringLiteral("leftSource"),
                model_.visualSourceFor(presentation.left_request_ticket),
            },
            {
                QStringLiteral("rightSource"),
                model_.visualSourceFor(presentation.right_request_ticket),
            },
        };
    } catch (const std::exception& error) {
    setComparisonStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Cannot prepare exact comparison · %1"),
        {QString::fromUtf8(error.what())})
        );
        return {};
    }
}

bool ReviewController::confirmComparisonReady(
    const QString& presentation_id,
    const QString& left_request_ticket,
    const QString& right_request_ticket
) {
    if (decision_session_.busy()) {
        return false;
    }
    if (presentation_id.trimmed().isEmpty()
        || left_request_ticket.trimmed().isEmpty()
        || right_request_ticket.trimmed().isEmpty()) {
        return false;
    }
    try {
        backend_->confirmReviewComparisonReady(
            presentation_id,
            left_request_ticket,
            right_request_ticket
        );
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Exact encoded artifacts and decoded Compare frames verified"))
        );
        return true;
    } catch (const std::exception& error) {
    setComparisonStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Comparison frame verification failed · %1"),
        {QString::fromUtf8(error.what())})
        );
        return false;
    }
}

void ReviewController::cancelComparison(const QString& presentation_id) {
    if (presentation_id.trimmed().isEmpty() || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    try {
        backend_->cancelReviewComparison(presentation_id);
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Comparison presentation closed")));
    } catch (const std::exception& error) {
    setComparisonStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Cannot close comparison presentation · %1"),
        {QString::fromUtf8(error.what())})
        );
    }
}

void ReviewController::recordComparison(
    const QString& presentation_id,
    const int outcome
) {
    const auto resolved_outcome = pairwise_outcome(outcome);
    if (!resolved_outcome) {
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Comparison outcome is not supported")));
        return;
    }
    if (presentation_id.trimmed().isEmpty()) {
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Prepare and verify the comparison first")));
        return;
    }
    if (decision_session_.busy() || scan_running_ || refreshing() || page_running_) {
        return;
    }
    if (!evidence_session_.beginRecord()) {
        return;
    }
    emit comparisonStateChanged();
  setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Recording append-only comparison evidence…")));
    evidence_watcher_.setFuture(QtConcurrent::run(
        record_comparison,
        backend_,
        presentation_id,
        *resolved_outcome
    ));
}

void ReviewController::undoLastComparison() {
    if (decision_session_.busy() || scan_running_ || refreshing() || page_running_) {
        return;
    }
    const auto event_id = evidence_session_.beginForget();
    if (!event_id) {
        return;
    }
    emit comparisonStateChanged();
  setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Appending a forget fact for the latest evidence…")));
    evidence_watcher_.setFuture(QtConcurrent::run(
        forget_comparison,
        backend_,
        *event_id
    ));
}

void ReviewController::setPhotoFlag(
    const QString& photo_id,
    const QString& flag
) {
    const auto desired_flag = decision_flag(flag);
    if (!desired_flag) {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController", "Unsupported Review flag")));
        return;
    }
    if (scan_running_ || refreshing() || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto current_value = model_.decisionFor(photo_id);
    if (!current_value) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Select a loaded photo before setting a flag")));
        return;
    }
    BackendReviewDecisionState current;
    try {
        current = backend_decision_state(photo_id, *current_value);
    } catch (const std::exception& error) {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Could not read the current flag decision · %1"),
        {QString::fromUtf8(error.what())}));
        return;
    }
    const auto request = decision_session_.beginSet(
        std::move(current),
        *desired_flag,
        static_cast<std::uint8_t>(current_value->rating)
    );
    if (!request) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Flag already matches the selected photo")));
        return;
    }
  setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Appending an explicit flag decision…")));
    startDecisionMutation(*request);
}

void ReviewController::setPhotoRating(
    const QString& photo_id,
    const int rating
) {
    if (rating < 0 || rating > 5) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Rating must be between 0 and 5 stars")));
        return;
    }
    if (scan_running_ || refreshing() || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto current_value = model_.decisionFor(photo_id);
    if (!current_value) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Select a loaded photo before setting a rating")));
        return;
    }
    BackendReviewDecisionState current;
    try {
        current = backend_decision_state(photo_id, *current_value);
    } catch (const std::exception& error) {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Could not read the current star rating · %1"),
        {QString::fromUtf8(error.what())}));
        return;
    }
    const BackendReviewDecisionFlag current_flag = current.flag;
    const auto request = decision_session_.beginSet(
        std::move(current),
        current_flag,
        static_cast<std::uint8_t>(rating)
    );
    if (!request) {
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Rating already matches the selected photo")));
        return;
    }
  setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Appending an explicit star rating…")));
    startDecisionMutation(*request);
}

void ReviewController::setPhotoColorLabel(
    const QString& photo_id,
    const QString& color_label
) {
    if (scan_running_ || refreshing() || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const QString normalized = color_label.trimmed().toLower();
    if (!model_.setColorLabel(photo_id, normalized)) {
        return;
    }
    persistColorLabels();
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Updated the local color label")));
    emit colorLabelChanged(photo_id, normalized);
}

void ReviewController::clearFilters() {
    filtered_model_.clearFilters();
}

void ReviewController::refreshVisibleLibrary() {
    if (scan_running_ || decision_session_.busy()) {
        return;
    }
    requestFinalPageRefresh();
}

void ReviewController::refreshSharedGradeNodes() {
    try {
        const auto refreshed = backend_->sharedGradeNodes();
        if (refreshed != shared_grade_nodes_) {
            shared_grade_nodes_ = refreshed;
            emit sharedGradeNodesChanged();
        }
    } catch (const std::exception& error) {
        setStatusMessage(review_message(
            QT_TRANSLATE_NOOP("ReviewController",
                              "Could not load shared Grade Nodes · %1"),
            {QString::fromUtf8(error.what())}
        ));
    }
}

QVariantMap ReviewController::applySharedGradeNode(
    const QString& layer_id,
    const QVariantList& targets
) {
    QVector<BackendBatchPhotoTarget> batch;
    batch.reserve(targets.size());
    QSet<QString> seen_photo_ids;
    for (const auto& value : targets) {
        const auto target = value.toMap();
        const QString photo_id = target.value(QStringLiteral("photoId")).toString();
        const QString source_path =
            target.value(QStringLiteral("sourcePath")).toString();
        if (photo_id.isEmpty() || source_path.isEmpty()
            || seen_photo_ids.contains(photo_id)) {
            continue;
        }
        seen_photo_ids.insert(photo_id);
        batch.push_back({
            .photo_id = photo_id,
            .source_path = source_path,
        });
    }
    if (layer_id.isEmpty() || batch.isEmpty()) {
        return {
            {QStringLiteral("requested"), 0},
            {QStringLiteral("updated"), 0},
            {QStringLiteral("unchanged"), 0},
            {QStringLiteral("failed"), 0},
            {QStringLiteral("errors"), QStringList{}},
        };
    }
    try {
        const auto receipt =
            backend_->applySharedGradeNodeToPhotos(layer_id, batch);
        QStringList errors;
        errors.reserve(receipt.errors.size());
        for (const auto& error : receipt.errors) {
            errors.push_back(error);
        }
        if (receipt.failed == 0) {
            setStatusMessage(review_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Shared Grade Node linked to %1 photos · %2 already current"
                ),
                {
                    static_cast<qulonglong>(receipt.updated),
                    static_cast<qulonglong>(receipt.unchanged),
                }
            ));
        } else {
            setStatusMessage(review_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Shared Grade Node linked to %1 photos · %2 failed"
                ),
                {
                    static_cast<qulonglong>(receipt.updated),
                    static_cast<qulonglong>(receipt.failed),
                }
            ));
        }
        if (receipt.updated > 0) {
            refreshVisibleLibrary();
        }
        return {
            {QStringLiteral("requested"), receipt.requested},
            {QStringLiteral("updated"), receipt.updated},
            {QStringLiteral("unchanged"), receipt.unchanged},
            {QStringLiteral("failed"), receipt.failed},
            {QStringLiteral("errors"), errors},
        };
    } catch (const std::exception& error) {
        const QString message = QString::fromUtf8(error.what());
        setStatusMessage(review_message(
            QT_TRANSLATE_NOOP(
                "ReviewController",
                "Could not apply shared Grade Node · %1"
            ),
            {message}
        ));
        return {
            {QStringLiteral("requested"), batch.size()},
            {QStringLiteral("updated"), 0},
            {QStringLiteral("unchanged"), 0},
            {QStringLiteral("failed"), batch.size()},
            {QStringLiteral("errors"), QStringList{message}},
        };
    }
}

void ReviewController::setFilterFlag(const QString& filter) {
    filtered_model_.setFlagFilter(filter);
}

void ReviewController::setFilterMinimumRating(const int rating) {
    filtered_model_.setMinimumRating(rating);
}

void ReviewController::setFilterColorLabel(const QString& color_label) {
    filtered_model_.setColorFilter(color_label);
}

void ReviewController::setFilterEditState(const QString& edit_state) {
    filtered_model_.setEditFilter(edit_state);
}

void ReviewController::undoLastDecision() {
    if (scan_running_ || refreshing() || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto request = decision_session_.beginUndo();
    if (!request) {
    setDecisionStatusMessage(review_message(
            decision_session_.undoDepth() > 0
                ? QT_TRANSLATE_NOOP("ReviewController",
                                "Local undo is disabled because the "
                                "authoritative decision changed")
                : QT_TRANSLATE_NOOP(
                  "ReviewController",
                  "No decision from this app session is available to undo"))
        );
        emit decisionStateChanged();
        return;
    }
  setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController", "Appending an inverse decision event…")));
    startDecisionMutation(*request);
}

void ReviewController::startDecisionMutation(
    const ReviewDecisionMutationRequest& request
) {
    emit decisionStateChanged();
    decision_watcher_.setFuture(QtConcurrent::run(
        run_decision_mutation,
        backend_,
        request
    ));
}

void ReviewController::finishScan() {
    const ScanTaskResult result = scan_watcher_.result();
    if (result.generation != scan_generation_) {
        return;
    }
    scan_progress_timer_.stop();
    pollScanProgress();
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    const bool old_refreshing = refreshing();
    terminal_refresh_active_ = true;
    final_page_refresh_pending_ = true;
    if (!result.error.isEmpty()) {
        scan_phase_ = BackendScanPhase::Failed;
        scan_terminal_error_ = result.error;
        scan_terminal_cancelled_ = false;
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
        scan_terminal_cancelled_ = result.report.cancelled;
        scan_phase_ = result.report.cancelled ? BackendScanPhase::Cancelled
                                              : BackendScanPhase::Completed;
        emit folderPathChanged();
    }
    scan_running_ = false;
    emit scanningChanged();
    emit scanProgressChanged();
    emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
    requestFinalPageRefresh();
}

void ReviewController::pollScanProgress() {
    if (scan_generation_ == 0) {
        return;
    }
    BackendScanProgress progress;
    try {
        progress = backend_->scanProgress(scan_generation_);
    } catch (const std::exception& error) {
        if (scan_running_) {
      setStatusMessage(
          review_message(QT_TRANSLATE_NOOP("ReviewController",
                                           "Import progress unavailable · %1"),
                         {QString::fromUtf8(error.what())})
            );
        }
        return;
    }
    if (!progress.valid || progress.scan_id != scan_generation_
        || progress.update_sequence <= scan_update_sequence_) {
        return;
    }

    scan_update_sequence_ = progress.update_sequence;
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
    scan_phase_ = progress.phase;
    emit scanProgressChanged();
    if (scan_running_) {
        updateScanStatus();
    }

    const quint64 catalogued = inserted_files_ + unchanged_files_ + revalidation_files_;
    const qint64 elapsed = scan_clock_.isValid() ? scan_clock_.elapsed() : 0;
    const bool first_visible_page = model_.rowCount() == 0 && catalogued > 0;
    const bool paced_refresh = catalogued >= next_stream_refresh_at_
        && (last_stream_refresh_ms_ < 0
            || elapsed - last_stream_refresh_ms_ >= STREAM_REFRESH_MIN_INTERVAL_MS);
    const bool visual_refresh = preview_artifacts_ready_ > last_stream_visual_refresh_at_
        && (last_stream_refresh_ms_ < 0
            || elapsed - last_stream_refresh_ms_ >= STREAM_VISUAL_REFRESH_MIN_INTERVAL_MS);
    if (scan_running_ && !page_running_
        && (first_visible_page || paced_refresh || visual_refresh)) {
        last_stream_refresh_ms_ = elapsed;
        next_stream_refresh_at_ = catalogued + STREAM_REFRESH_STRIDE;
        last_stream_visual_refresh_at_ = preview_artifacts_ready_;
        startPage(PageTaskKind::StreamingPrefix);
    }
}

void ReviewController::requestFinalPageRefresh() {
    terminal_refresh_active_ = true;
    final_page_refresh_pending_ = true;
    if (page_running_) {
        return;
    }
    final_page_refresh_pending_ = false;
    startPage(PageTaskKind::FinalReset);
}

void ReviewController::finishPage() {
    PageTaskResult result = page_watcher_.result();
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    const bool old_refreshing = refreshing();
    page_running_ = false;
    page_reset_running_ = false;
    if (result.library_generation != library_generation_
        || result.request_id != active_page_request_id_) {
        emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
        return;
    }

    const auto finish_failure = [this, &result, old_busy, old_loading_more,
                                 old_refreshing](QString error) {
        if (result.kind != PageTaskKind::FinalReset
            && final_page_refresh_pending_ && !scan_running_) {
      setStatusMessage(review_message(
          QT_TRANSLATE_NOOP("ReviewController",
                            "Live Library refresh failed · rebuilding one "
                            "stable final view · %1"),
          {std::move(error)})
            );
            emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
            final_page_refresh_pending_ = false;
            startPage(PageTaskKind::FinalReset);
            return;
        }
        if (result.kind == PageTaskKind::FinalReset) {
            final_page_refresh_pending_ = false;
            terminal_refresh_active_ = false;
            setHasMore(false);
      setStatusMessage(review_message(
          QT_TRANSLATE_NOOP(
              "ReviewController",
              "Final Library refresh failed · visible photos retained · add "
              "the folder again or reopen Shadow to retry · %1"),
          {std::move(error)})
            );
        } else if (scan_running_) {
      setStatusMessage(review_message(
          QT_TRANSLATE_NOOP("ReviewController",
                            "Live Library refresh delayed · import is still "
                            "safe and continuing · %1"),
          {std::move(error)})
            );
        } else {
      setStatusMessage(review_message(
          QT_TRANSLATE_NOOP(
              "ReviewController",
              "Library refresh failed · existing photos retained · %1"),
          {std::move(error)})
            );
        }
        emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
    };

    if (!result.error.isEmpty()) {
        finish_failure(result.error);
        return;
    }
    if (result.page.has_more
        && (result.page.items.isEmpty() || result.page.next_cursor_path.isEmpty()
            || result.page.next_cursor_representation_id.isEmpty())) {
        finish_failure(QStringLiteral("the page exposed an invalid continuation cursor"));
        return;
    }

    auto items = review_items(std::move(result.page.items));
    QVector<BackendReviewDecisionState> decision_states;
    decision_states.reserve(items.size());
    try {
        for (const auto& item : items) {
            decision_states.push_back(backend_decision_state(
                item.photo_id,
                {
                    .head_sequence = item.decision_head_sequence,
                    .flag = item.decision_flag,
                    .rating = item.decision_rating,
                }
            ));
        }
    } catch (const std::exception& error) {
        finish_failure(QString::fromUtf8(error.what()));
        return;
    }
    bool accepted = false;
    switch (result.kind) {
    case PageTaskKind::InitialReset:
    case PageTaskKind::FinalReset:
        accepted = model_.reconcileSnapshot(std::move(items), library_generation_);
        break;
    case PageTaskKind::StreamingPrefix:
        accepted = model_.reconcilePrefixSnapshot(
            std::move(items),
            library_generation_
        );
        break;
    case PageTaskKind::Append:
        accepted = model_.appendSnapshot(std::move(items), library_generation_);
        break;
    }
    if (!accepted) {
        finish_failure(QStringLiteral("the page contained invalid or duplicate stable keys"));
        return;
    }

    for (auto& state : decision_states) {
        decision_session_.reconcile(std::move(state));
    }
    total_items_ = result.page.total_items;
    if (result.kind == PageTaskKind::StreamingPrefix) {
        setHasMore(false);
    } else {
        next_cursor_path_ = std::move(result.page.next_cursor_path);
        next_cursor_representation_id_ =
            std::move(result.page.next_cursor_representation_id);
        setHasMore(result.page.has_more);
    }
    if (result.kind == PageTaskKind::FinalReset) {
        final_page_refresh_pending_ = false;
        terminal_refresh_active_ = false;
    }
    emit itemCountChanged();
    emit decisionStateChanged();
    if (result.kind != PageTaskKind::FinalReset
        && final_page_refresh_pending_ && !scan_running_) {
        emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
        final_page_refresh_pending_ = false;
        startPage(PageTaskKind::FinalReset);
        return;
    }
    if (scan_running_) {
        updateScanStatus();
        emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
        return;
    }
    updateReadyStatus();
    emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
}

void ReviewController::finishEvidenceTask() {
    ReviewEvidenceTaskResult result = evidence_watcher_.result();
    if (!result.error.isEmpty()) {
        evidence_session_.fail();
        emit comparisonStateChanged();
    setComparisonStatusMessage(review_message(result.kind == ReviewEvidenceTaskKind::Record
                 ? QT_TRANSLATE_NOOP("ReviewController",
                                "Evidence write failed · pair retained · %1")
                 : QT_TRANSLATE_NOOP("ReviewController",
                                "Forget write failed · evidence retained · %1"),
        {result.error})
        );
        return;
    }

    if (result.kind == ReviewEvidenceTaskKind::Record) {
        if (!evidence_session_.completeRecord(result.feedback.event_id)) {
            emit comparisonStateChanged();
      setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
          "ReviewController", "Evidence receipt was invalid; pair retained")));
            return;
        }
        emit comparisonStateChanged();
    setComparisonStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Preference evidence recorded · sequence %1 · model not active"),
        {result.feedback.sequence})
        );
        emit comparisonRecorded();
        return;
    }

    if (!evidence_session_.completeForget(result.forget.target_event_id)) {
        emit comparisonStateChanged();
    setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController", "Forget receipt was invalid; evidence retained")));
        return;
    }
    emit comparisonStateChanged();
  setComparisonStatusMessage(review_message(QT_TRANSLATE_NOOP(
      "ReviewController",
      "Latest evidence forgotten non-destructively · source event retained"))
    );
    emit comparisonForgotten();
}

void ReviewController::finishDecisionTask() {
    const ReviewDecisionTaskResult result = decision_watcher_.result();
    if (!result.error.isEmpty()) {
        decision_session_.fail();
        if (result.has_authoritative) {
            applyDecisionState(result.authoritative);
        }
        emit decisionStateChanged();
        if (result.is_undo && result.has_authoritative
            && !decision_session_.canUndo()) {
      setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
          "ReviewController", "Undo blocked · authoritative decision changed "
                              "outside this session"))
            );
        } else if (result.is_undo && decision_session_.canUndo()) {
      setDecisionStatusMessage(review_message(
          QT_TRANSLATE_NOOP(
              "ReviewController",
              "Undo write failed · unchanged state remains retryable · %1"),
          {result.error})
            );
        } else if (result.has_authoritative) {
      setDecisionStatusMessage(review_message(
          QT_TRANSLATE_NOOP(
              "ReviewController",
              "Decision write failed · authoritative state refreshed · %1"),
          {result.error})
            );
        } else {
      setDecisionStatusMessage(review_message(
          QT_TRANSLATE_NOOP(
              "ReviewController",
              "Decision write failed · refresh also failed · %1 · %2"),
          {result.error, result.refresh_error})
            );
        }
        return;
    }

    if (!decision_session_.complete(result.receipt)) {
        emit decisionStateChanged();
    setDecisionStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Decision receipt was invalid; local state retained")));
        return;
    }
    applyDecisionState(result.receipt.after);
    emit decisionStateChanged();
    if (result.is_undo) {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Inverse decision appended · sequence %1 · history retained"),
        {result.receipt.sequence})
        );
        emit decisionUndone();
    } else {
    setDecisionStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Manual decision recorded · sequence %1"),
        {result.receipt.sequence})
        );
    }
}

void ReviewController::startPage(const PageTaskKind kind) {
    if (page_running_ || decision_session_.busy()) {
        return;
    }
    const bool reset = kind != PageTaskKind::Append;
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    const bool old_refreshing = refreshing();
    page_running_ = true;
    page_reset_running_ = reset;
    active_page_request_id_ = ++page_request_id_;
    emitWorkStateChanges(old_busy, old_loading_more, old_refreshing);
    if (reset) {
        if (scan_running_) {
            updateScanStatus();
        } else if (!scan_terminal_error_.isEmpty()) {
      setStatusMessage(review_message(QT_TRANSLATE_NOOP(
          "ReviewController",
          "Refreshing photos retained before import stopped…")));
        } else if (scan_terminal_cancelled_) {
      setStatusMessage(review_message(QT_TRANSLATE_NOOP(
          "ReviewController",
          "Refreshing photos retained before import was cancelled…")));
        } else if (kind == PageTaskKind::FinalReset) {
      setStatusMessage(review_message(QT_TRANSLATE_NOOP(
          "ReviewController",
          "Rebuilding one stable Library view before paging…")));
        } else {
      setStatusMessage(review_message(
          QT_TRANSLATE_NOOP("ReviewController", "Loading the local Library…")));
        }
    } else {
        updateReadyStatus();
    }
    const QVector<QString> required_representation_ids =
        kind == PageTaskKind::FinalReset ? model_.representationIds()
                                         : QVector<QString>{};
    page_watcher_.setFuture(QtConcurrent::run([
        backend = backend_,
        cursor_path = reset ? QString{} : next_cursor_path_,
        cursor_id = reset ? QString{} : next_cursor_representation_id_,
        library_generation = library_generation_,
        request_id = active_page_request_id_,
        kind,
        required_representation_ids
    ]() {
        return run_page(
            backend,
            cursor_path,
            cursor_id,
            library_generation,
            request_id,
            kind,
            required_representation_ids
        );
    }));
}

void ReviewController::emitWorkStateChanges(
    const bool old_busy,
    const bool old_loading_more,
    const bool old_refreshing
) {
    if (old_busy != busy()) {
        emit busyChanged();
    }
    if (old_loading_more != loadingMore()) {
        emit loadingMoreChanged();
    }
    if (old_refreshing != refreshing()) {
        emit refreshingChanged();
    }
}

void ReviewController::setHasMore(const bool has_more) {
    if (has_more_ == has_more) {
        return;
    }
    has_more_ = has_more;
    emit hasMoreChanged();
}

bool ReviewController::eventFilter(QObject *const watched,
                                   QEvent *const event) {
  if (watched == QCoreApplication::instance() &&
      event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  return QObject::eventFilter(watched, event);
}

void ReviewController::retranslateUi() {
  emit statusTextChanged();
  emit comparisonStatusTextChanged();
  emit decisionStatusTextChanged();
}

void ReviewController::setStatusMessage(LocalizedUiMessage status) {
    if (status_message_ == status) {
        return;
    }
  status_message_ = std::move(status);
    emit statusTextChanged();
}

void ReviewController::updateReadyStatus() {
    if (!scan_terminal_error_.isEmpty()) {
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Import stopped · %1 photos remain available · filesystem/import "
            "error: %2 · %3 decode failures · %4 preview failures"),
        {
            total_items_,
            scan_terminal_error_,
            decode_hard_failures_,
            preview_failures_,
        })
        );
        return;
    }
    if (scan_terminal_cancelled_) {
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Import cancelled · %1 photos remain available · %2 "
                          "filesystem issues · %3 decode failures · %4 preview "
                          "failures · %5 decode jobs cancelled"),
        {
            total_items_,
            issue_count_,
            decode_hard_failures_,
            preview_failures_,
            decode_cancelled_,
        })
        );
        return;
    }
    if (total_items_ == 0) {
    setStatusMessage(review_message(QT_TRANSLATE_NOOP(
        "ReviewController",
        "Local Library is empty · add a photo folder to begin")));
        return;
    }
    const char *const source = loadingMore() ? QT_TRANSLATE_NOOP(
                "ReviewController",
                "%1 / %2 loaded · %3 supported · %4/%5 preview checks "
                "completed · %6 filesystem issues · %7 decode failures · %8 "
                "preview failures · loading more") : QT_TRANSLATE_NOOP("ReviewController",
                              "%1 / %2 loaded · %3 supported · %4/%5 preview "
                              "checks completed · %6 filesystem issues · %7 "
                              "decode failures · %8 preview failures");
  setStatusMessage(review_message(source, {
                                              model_.rowCount(),
                                              total_items_,
                                              supported_files_,
                                              decode_completed_,
                                              decode_queued_,
                                              issue_count_,
                                              decode_hard_failures_,
                                              preview_failures_,
                                          })
    );
}

void ReviewController::updateScanStatus() {
    const quint64 catalogued = inserted_files_ + unchanged_files_ + revalidation_files_;
    switch (scan_phase_) {
    case BackendScanPhase::PreparingPreviews:
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP("ReviewController",
                          "Import catalogued %1 files · finishing %2 queued "
                          "preview checks · %3 filesystem issues"),
        {catalogued, decode_queued_, issue_count_})
        );
        break;
    case BackendScanPhase::Cancelling:
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Stopping import safely · %1 files retained · queued checks are "
            "being cancelled · current preview may finish"),
        {catalogued})
        );
        break;
    case BackendScanPhase::Discovering:
    default:
    setStatusMessage(review_message(
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Importing · %1 files checked · %2 supported · %3 catalogued · %4 "
            "preview checks queued · %5 filesystem issues"),
        {
            files_seen_,
            supported_files_,
            catalogued,
            decode_queued_,
            issue_count_,
        })
        );
        break;
    }
}

void ReviewController::setComparisonStatusMessage(LocalizedUiMessage status) {
    if (comparison_status_message_ == status) {
        return;
    }
  comparison_status_message_ = std::move(status);
    emit comparisonStatusTextChanged();
}

void ReviewController::setDecisionStatusMessage(LocalizedUiMessage status) {
    if (decision_status_message_ == status) {
        return;
    }
  decision_status_message_ = std::move(status);
    emit decisionStatusTextChanged();
}

void ReviewController::applyDecisionState(
    const BackendReviewDecisionState& state
) {
    decision_session_.reconcile(state);
    const QString flag = decision_flag_name(state.flag);
    const int rating = static_cast<int>(state.rating);
    const bool projected = model_.updateDecision(
        state.photo_id,
        state.head_sequence,
        flag,
        rating
    );
    (void)projected;
    emit decisionChanged(state.photo_id, state.head_sequence, flag, rating);
}

void ReviewController::persistColorLabels() {
    settings_->setValue(
        QString::fromLatin1(color_labels_settings_key),
        model_.colorLabels()
    );
    settings_->sync();
}
