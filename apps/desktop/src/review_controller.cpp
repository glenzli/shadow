#include "review_controller.hpp"

#include <QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t REVIEW_PAGE_SIZE = 96;

[[nodiscard]] ScanTaskResult run_scan(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& folder_path,
    const quint64 generation
) {
    ScanTaskResult result;
    result.generation = generation;
    try {
        result.report = backend->scanFolder(folder_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

[[nodiscard]] PageTaskResult run_page(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& cursor_path,
    const QString& cursor_representation_id,
    const quint64 generation,
    const bool reset
) {
    PageTaskResult result;
    result.generation = generation;
    result.reset = reset;
    try {
        result.page = backend->reviewPage(
            cursor_path,
            cursor_representation_id,
            REVIEW_PAGE_SIZE
        );
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
            .title = std::move(item.title),
            .source_path = std::move(item.source_path),
            .visual_role = std::move(item.visual_role),
            .visual_width = item.visual_width,
            .visual_height = item.visual_height,
            .has_visual = item.has_visual,
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
    QObject* parent
)
    : QObject(parent), backend_(std::move(backend)), model_(this) {
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
}

ReviewController::~ReviewController() {
    scan_watcher_.waitForFinished();
    page_watcher_.waitForFinished();
    evidence_watcher_.waitForFinished();
    decision_watcher_.waitForFinished();
}

bool ReviewController::busy() const noexcept {
    return scan_running_ || (page_running_ && model_.rowCount() == 0);
}

bool ReviewController::loadingMore() const noexcept {
    return page_running_ && model_.rowCount() > 0;
}

bool ReviewController::hasMore() const noexcept {
    return has_more_;
}

QString ReviewController::folderPath() const {
    return folder_path_;
}

QString ReviewController::statusText() const {
    return status_text_;
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
    return comparison_status_text_;
}

bool ReviewController::decisionBusy() const noexcept {
    return decision_session_.busy();
}

bool ReviewController::canUndoDecision() const {
    return decision_session_.canUndo();
}

QString ReviewController::decisionStatusText() const {
    return decision_status_text_;
}

QAbstractItemModel* ReviewController::model() noexcept {
    return &model_;
}

ReviewModel* ReviewController::reviewModel() noexcept {
    return &model_;
}

void ReviewController::scanFolder(const QUrl& folder_url) {
    if (scan_running_ || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const QString path = folder_url.toLocalFile();
    if (path.isEmpty()) {
        setStatusText(QStringLiteral("The selected folder is not a local path"));
        return;
    }

    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    ++generation_;
    model_.replace({}, generation_);
    total_items_ = 0;
    supported_files_ = 0;
    decode_queued_ = 0;
    issue_count_ = 0;
    next_cursor_path_.clear();
    next_cursor_representation_id_.clear();
    setHasMore(false);
    emit itemCountChanged();
    folder_path_ = path;
    emit folderPathChanged();
    scan_running_ = true;
    emitWorkStateChanges(old_busy, old_loading_more);
    setStatusText(QStringLiteral("Scanning RAW files and preparing Review previews…"));
    scan_watcher_.setFuture(QtConcurrent::run(
        [backend = backend_, folder = folder_path_, generation = generation_]() {
            return run_scan(backend, folder, generation);
        }
    ));
}

void ReviewController::loadMore() {
    if (!has_more_ || scan_running_ || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    startPage(false);
}

QVariantMap ReviewController::prepareComparison(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) {
    if (evidence_session_.busy() || decision_session_.busy() || scan_running_
        || page_running_) {
        return {};
    }
    if (left_visual_handle.trimmed().isEmpty()
        || right_visual_handle.trimmed().isEmpty()) {
        setComparisonStatusText(QStringLiteral("Choose two verified visuals before comparing"));
        return {};
    }
    try {
        const auto presentation = backend_->prepareReviewComparison(
            left_visual_handle,
            right_visual_handle
        );
        setComparisonStatusText(
            QStringLiteral("Loading two exact Compare frames with durable provenance…")
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
        setComparisonStatusText(
            QStringLiteral("Cannot prepare exact comparison · %1")
                .arg(QString::fromUtf8(error.what()))
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
        setComparisonStatusText(
            QStringLiteral("Exact encoded artifacts and decoded Compare frames verified")
        );
        return true;
    } catch (const std::exception& error) {
        setComparisonStatusText(
            QStringLiteral("Comparison frame verification failed · %1")
                .arg(QString::fromUtf8(error.what()))
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
        setComparisonStatusText(QStringLiteral("Comparison presentation closed"));
    } catch (const std::exception& error) {
        setComparisonStatusText(
            QStringLiteral("Cannot close comparison presentation · %1")
                .arg(QString::fromUtf8(error.what()))
        );
    }
}

void ReviewController::recordComparison(
    const QString& presentation_id,
    const int outcome
) {
    const auto resolved_outcome = pairwise_outcome(outcome);
    if (!resolved_outcome) {
        setComparisonStatusText(QStringLiteral("Comparison outcome is not supported"));
        return;
    }
    if (presentation_id.trimmed().isEmpty()) {
        setComparisonStatusText(QStringLiteral("Prepare and verify the comparison first"));
        return;
    }
    if (decision_session_.busy() || scan_running_ || page_running_) {
        return;
    }
    if (!evidence_session_.beginRecord()) {
        return;
    }
    emit comparisonStateChanged();
    setComparisonStatusText(QStringLiteral("Recording append-only comparison evidence…"));
    evidence_watcher_.setFuture(QtConcurrent::run(
        record_comparison,
        backend_,
        presentation_id,
        *resolved_outcome
    ));
}

void ReviewController::undoLastComparison() {
    if (decision_session_.busy() || scan_running_ || page_running_) {
        return;
    }
    const auto event_id = evidence_session_.beginForget();
    if (!event_id) {
        return;
    }
    emit comparisonStateChanged();
    setComparisonStatusText(QStringLiteral("Appending a forget fact for the latest evidence…"));
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
        setDecisionStatusText(QStringLiteral("Unsupported Review flag"));
        return;
    }
    if (scan_running_ || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto current_value = model_.decisionFor(photo_id);
    if (!current_value) {
        setDecisionStatusText(QStringLiteral("Select a loaded photo before setting a flag"));
        return;
    }
    BackendReviewDecisionState current;
    try {
        current = backend_decision_state(photo_id, *current_value);
    } catch (const std::exception& error) {
        setDecisionStatusText(QString::fromUtf8(error.what()));
        return;
    }
    const auto request = decision_session_.beginSet(
        std::move(current),
        *desired_flag,
        static_cast<std::uint8_t>(current_value->rating)
    );
    if (!request) {
        setDecisionStatusText(QStringLiteral("Flag already matches the selected photo"));
        return;
    }
    setDecisionStatusText(QStringLiteral("Appending an explicit flag decision…"));
    startDecisionMutation(*request);
}

void ReviewController::setPhotoRating(
    const QString& photo_id,
    const int rating
) {
    if (rating < 0 || rating > 5) {
        setDecisionStatusText(QStringLiteral("Rating must be between 0 and 5 stars"));
        return;
    }
    if (scan_running_ || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto current_value = model_.decisionFor(photo_id);
    if (!current_value) {
        setDecisionStatusText(QStringLiteral("Select a loaded photo before setting a rating"));
        return;
    }
    BackendReviewDecisionState current;
    try {
        current = backend_decision_state(photo_id, *current_value);
    } catch (const std::exception& error) {
        setDecisionStatusText(QString::fromUtf8(error.what()));
        return;
    }
    const BackendReviewDecisionFlag current_flag = current.flag;
    const auto request = decision_session_.beginSet(
        std::move(current),
        current_flag,
        static_cast<std::uint8_t>(rating)
    );
    if (!request) {
        setDecisionStatusText(QStringLiteral("Rating already matches the selected photo"));
        return;
    }
    setDecisionStatusText(QStringLiteral("Appending an explicit star rating…"));
    startDecisionMutation(*request);
}

void ReviewController::undoLastDecision() {
    if (scan_running_ || page_running_ || evidence_session_.busy()
        || decision_session_.busy()) {
        return;
    }
    const auto request = decision_session_.beginUndo();
    if (!request) {
        setDecisionStatusText(
            decision_session_.undoDepth() > 0
                ? QStringLiteral(
                      "Local undo is disabled because the authoritative decision changed"
                  )
                : QStringLiteral("No decision from this app session is available to undo")
        );
        emit decisionStateChanged();
        return;
    }
    setDecisionStatusText(QStringLiteral("Appending an inverse decision event…"));
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
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    scan_running_ = false;
    emitWorkStateChanges(old_busy, old_loading_more);
    if (result.generation != generation_) {
        return;
    }
    if (!result.error.isEmpty()) {
        setStatusText(QStringLiteral("Scan failed · %1").arg(result.error));
        return;
    }
    folder_path_ = result.report.folder_path;
    supported_files_ = result.report.supported_files;
    decode_queued_ = result.report.decode_queued;
    issue_count_ = result.report.issue_count;
    emit folderPathChanged();
    startPage(true);
}

void ReviewController::finishPage() {
    PageTaskResult result = page_watcher_.result();
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    page_running_ = false;
    emitWorkStateChanges(old_busy, old_loading_more);
    if (result.generation != generation_) {
        return;
    }
    if (!result.error.isEmpty()) {
        setStatusText(QStringLiteral("Review page failed · %1").arg(result.error));
        return;
    }

    total_items_ = result.page.total_items;
    next_cursor_path_ = std::move(result.page.next_cursor_path);
    next_cursor_representation_id_ =
        std::move(result.page.next_cursor_representation_id);
    setHasMore(result.page.has_more);
    auto items = review_items(std::move(result.page.items));
    for (const auto& item : items) {
        decision_session_.reconcile(backend_decision_state(
            item.photo_id,
            {
                .head_sequence = item.decision_head_sequence,
                .flag = item.decision_flag,
                .rating = item.decision_rating,
            }
        ));
    }
    if (result.reset) {
        model_.replace(std::move(items), generation_);
    } else {
        model_.append(std::move(items));
    }
    emit itemCountChanged();
    emit decisionStateChanged();
    updateReadyStatus();
}

void ReviewController::finishEvidenceTask() {
    ReviewEvidenceTaskResult result = evidence_watcher_.result();
    if (!result.error.isEmpty()) {
        evidence_session_.fail();
        emit comparisonStateChanged();
        setComparisonStatusText(
            (result.kind == ReviewEvidenceTaskKind::Record
                 ? QStringLiteral("Evidence write failed · pair retained · %1")
                 : QStringLiteral("Forget write failed · evidence retained · %1"))
                .arg(result.error)
        );
        return;
    }

    if (result.kind == ReviewEvidenceTaskKind::Record) {
        if (!evidence_session_.completeRecord(result.feedback.event_id)) {
            emit comparisonStateChanged();
            setComparisonStatusText(QStringLiteral("Evidence receipt was invalid; pair retained"));
            return;
        }
        emit comparisonStateChanged();
        setComparisonStatusText(
            QStringLiteral("Preference evidence recorded · sequence %1 · model not active")
                .arg(result.feedback.sequence)
        );
        emit comparisonRecorded();
        return;
    }

    if (!evidence_session_.completeForget(result.forget.target_event_id)) {
        emit comparisonStateChanged();
        setComparisonStatusText(QStringLiteral("Forget receipt was invalid; evidence retained"));
        return;
    }
    emit comparisonStateChanged();
    setComparisonStatusText(
        QStringLiteral("Latest evidence forgotten non-destructively · source event retained")
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
            setDecisionStatusText(
                QStringLiteral(
                    "Undo blocked · authoritative decision changed outside this session"
                )
            );
        } else if (result.is_undo && decision_session_.canUndo()) {
            setDecisionStatusText(
                QStringLiteral("Undo write failed · unchanged state remains retryable · %1")
                    .arg(result.error)
            );
        } else if (result.has_authoritative) {
            setDecisionStatusText(
                QStringLiteral("Decision write failed · authoritative state refreshed · %1")
                    .arg(result.error)
            );
        } else {
            setDecisionStatusText(
                QStringLiteral("Decision write failed · refresh also failed · %1 · %2")
                    .arg(result.error, result.refresh_error)
            );
        }
        return;
    }

    if (!decision_session_.complete(result.receipt)) {
        emit decisionStateChanged();
        setDecisionStatusText(QStringLiteral("Decision receipt was invalid; local state retained"));
        return;
    }
    applyDecisionState(result.receipt.after);
    emit decisionStateChanged();
    if (result.is_undo) {
        setDecisionStatusText(
            QStringLiteral("Inverse decision appended · sequence %1 · history retained")
                .arg(result.receipt.sequence)
        );
        emit decisionUndone();
    } else {
        setDecisionStatusText(
            QStringLiteral("Manual decision recorded · sequence %1")
                .arg(result.receipt.sequence)
        );
    }
}

void ReviewController::startPage(const bool reset) {
    if (page_running_ || decision_session_.busy()) {
        return;
    }
    const bool old_busy = busy();
    const bool old_loading_more = loadingMore();
    page_running_ = true;
    emitWorkStateChanges(old_busy, old_loading_more);
    if (reset) {
        setStatusText(QStringLiteral("Loading the first Review page…"));
    } else {
        updateReadyStatus();
    }
    page_watcher_.setFuture(QtConcurrent::run([
        backend = backend_,
        cursor_path = reset ? QString{} : next_cursor_path_,
        cursor_id = reset ? QString{} : next_cursor_representation_id_,
        generation = generation_,
        reset
    ]() { return run_page(backend, cursor_path, cursor_id, generation, reset); }));
}

void ReviewController::emitWorkStateChanges(
    const bool old_busy,
    const bool old_loading_more
) {
    if (old_busy != busy()) {
        emit busyChanged();
    }
    if (old_loading_more != loadingMore()) {
        emit loadingMoreChanged();
    }
}

void ReviewController::setHasMore(const bool has_more) {
    if (has_more_ == has_more) {
        return;
    }
    has_more_ = has_more;
    emit hasMoreChanged();
}

void ReviewController::setStatusText(QString status) {
    if (status_text_ == status) {
        return;
    }
    status_text_ = std::move(status);
    emit statusTextChanged();
}

void ReviewController::updateReadyStatus() {
    const QString loading = loadingMore() ? QStringLiteral(" · loading more") : QString{};
    setStatusText(
        QStringLiteral("%1 / %2 loaded · %3 supported · %4 rebuilt · %5 issues%6")
            .arg(model_.rowCount())
            .arg(total_items_)
            .arg(supported_files_)
            .arg(decode_queued_)
            .arg(issue_count_)
            .arg(loading)
    );
}

void ReviewController::setComparisonStatusText(QString status) {
    if (comparison_status_text_ == status) {
        return;
    }
    comparison_status_text_ = std::move(status);
    emit comparisonStatusTextChanged();
}

void ReviewController::setDecisionStatusText(QString status) {
    if (decision_status_text_ == status) {
        return;
    }
    decision_status_text_ = std::move(status);
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
