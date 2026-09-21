#include "people_analysis_controller.hpp"

#include "ai_preferences.hpp"

#include <QtConcurrentRun>

#include <QDebug>
#include <QSet>
#include <QVariantMap>

#include <algorithm>
#include <exception>
#include <utility>

namespace {

[[nodiscard]] QString thumbnail_source(const QByteArray& jpeg) {
    if (jpeg.isEmpty()) {
        return {};
    }
    return QStringLiteral("data:image/jpeg;base64,%1").arg(QString::fromLatin1(jpeg.toBase64()));
}

} // namespace

PeopleAnalysisController::PeopleAnalysisController(
    Operations operations,
    AiPreferences* const preferences,
    QObject* const parent
) : QObject(parent), operations_(std::move(operations)), preferences_(preferences) {
    Q_ASSERT(preferences_ != nullptr);
    progress_timer_.setInterval(125);
    progress_timer_.setTimerType(Qt::CoarseTimer);
    connect(&progress_timer_, &QTimer::timeout, this, &PeopleAnalysisController::pollProgress);
    connect(
        &watcher_,
        &QFutureWatcher<PeopleAnalysisTaskResult>::finished,
        this,
        &PeopleAnalysisController::finishAnalysis
    );
    connect(preferences_, &AiPreferences::peopleAnalysisConsentChanged, this, [this]() {
        if (!preferences_->peopleAnalysisExecutionAllowed() && busy()) {
            cancelAnalysis();
        }
        emit stateChanged();
    });
    try {
        report_ = operations_.load();
        has_results_ = report_.has_data;
        if (has_results_) {
            state_ = State::Ready;
        }
    } catch (const std::exception& error) {
        qWarning().noquote() << "Local people data could not be loaded:" << error.what();
        state_ = State::Failed;
    }
}

PeopleAnalysisController::~PeopleAnalysisController() {
    if (active_job_token_.has_value()) {
        try {
            (void)operations_.cancel(*active_job_token_);
        } catch (const std::exception& error) {
            qWarning().noquote() << "People analysis shutdown cancellation failed:" << error.what();
        }
    }
    watcher_.waitForFinished();
    if (active_job_token_.has_value()) {
        retireJob(*active_job_token_);
    }
}

bool PeopleAnalysisController::busy() const noexcept {
    return active_job_token_.has_value() || continuation_queued_;
}

bool PeopleAnalysisController::cancelRequested() const noexcept {
    return state_ == State::Cancelling || progress_.cancellation_requested;
}

bool PeopleAnalysisController::hasResults() const noexcept {
    return has_results_;
}

QString PeopleAnalysisController::statusText() const {
    switch (state_) {
    case State::Idle:
        return preferences_->peopleAnalysisExecutionAllowed()
                   ? tr("Ready to organize people locally.")
                   : tr("People analysis is off. Enable it to begin.");
    case State::AuthorizationRequired:
        return tr("Allow local people analysis before starting.");
    case State::Running:
        if (progress_.phase == QStringLiteral("grouping")) {
            return tr(
                "Preparing anonymous groups from %n compared faces…",
                nullptr,
                static_cast<int>(progress_.compared_faces)
            );
        }
        if (progress_.analyzed_photos > 0) {
            return tr("Analyzing photo %1 of at most %2 · %3 faces · %4 compared")
                .arg(progress_.analyzed_photos)
                .arg(progress_.maximum_photos)
                .arg(progress_.detected_faces)
                .arg(progress_.compared_faces);
        }
        return tr("Preparing local people analysis…");
    case State::Cancelling:
        return tr("Stopping after the current model request…");
    case State::Cancelled:
        return has_results_ ? tr("Analysis stopped. Stored people data was kept.")
                            : tr("Analysis stopped.");
    case State::Ready:
        return tr("Local people data is ready.");
    case State::Failed:
        return tr("Local people analysis could not finish.");
    }
    return {};
}

QString PeopleAnalysisController::errorText() const {
    if (state_ != State::Failed) {
        return {};
    }
    return tr(
        "Make sure Infer Runtime is running and Shadow access is configured, then try again."
    );
}

QVariantList PeopleAnalysisController::groups() const {
    QVariantList projected;
    projected.reserve(report_.groups.size());
    for (qsizetype index = 0; index < report_.groups.size(); ++index) {
        const BackendPeopleGroup& group = report_.groups.at(index);
        projected.push_back(
            QVariantMap{
                {QStringLiteral("groupId"), group.group_id},
                {QStringLiteral("displayName"), group.display_name},
                {QStringLiteral("displayIndex"), index + 1},
                {QStringLiteral("photoCount"), group.member_count},
                {QStringLiteral("thumbnailSource"), thumbnail_source(group.thumbnail_jpeg)},
                {QStringLiteral("selected"), selected_group_ids_.contains(group.group_id)},
                {QStringLiteral("merged"), group.manually_merged},
            }
        );
    }
    return projected;
}

uint PeopleAnalysisController::analyzedPhotos() const noexcept {
    return report_.analyzed_photos;
}

uint PeopleAnalysisController::detectedFaces() const noexcept {
    return report_.detected_faces;
}

uint PeopleAnalysisController::embeddedFaces() const noexcept {
    return report_.embedded_faces;
}

uint PeopleAnalysisController::skippedItems() const noexcept {
    return report_.skipped_items;
}

uint PeopleAnalysisController::ungroupedFaces() const noexcept {
    return report_.ungrouped_faces;
}

bool PeopleAnalysisController::truncated() const noexcept {
    return report_.truncated;
}

int PeopleAnalysisController::selectedGroupCount() const noexcept {
    return static_cast<int>(selected_group_ids_.size());
}

bool PeopleAnalysisController::canMergeSelectedGroups() const noexcept {
    return !busy() && selected_group_ids_.size() >= 2 && !selectedGroupsConflict();
}

bool PeopleAnalysisController::canUndoMerge() const noexcept {
    return report_.can_undo_merge;
}

QString PeopleAnalysisController::mergeSelectionText() const {
    if (selected_group_ids_.isEmpty()) {
        return tr("Select at least two people that should be one person.");
    }
    if (selected_group_ids_.size() == 1) {
        return tr("Select one more person to merge.");
    }
    if (selectedGroupsConflict()) {
        return tr("These groups contain faces from the same photo and cannot be merged.");
    }
    return tr(
        "%n people selected for merging.",
        nullptr,
        static_cast<int>(selected_group_ids_.size())
    );
}

void PeopleAnalysisController::reanalyzeAll() {
    if (busy() || !preferences_->peopleAnalysisExecutionAllowed() || !operations_.reset_scan)
        return;
    try {
        operations_.reset_scan();
    } catch (const std::exception& error) {
        qWarning().noquote() << "People analysis reset failed:" << error.what();
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    startAnalysis();
}

void PeopleAnalysisController::startAnalysis() {
    if (busy()) {
        return;
    }
    if (!preferences_->peopleAnalysisExecutionAllowed()) {
        state_ = State::AuthorizationRequired;
        emit stateChanged();
        emit authorizationRequired();
        return;
    }
    std::uint64_t job_token = 0;
    try {
        job_token = operations_.begin(true);
    } catch (const std::exception& error) {
        qWarning().noquote() << "People analysis registration failed:" << error.what();
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    active_job_token_ = job_token;
    progress_ = {.job_token = job_token};
    state_ = State::Running;
    emit stateChanged();
    watcher_.setFuture(QtConcurrent::run([execute = operations_.execute, job_token]() {
        PeopleAnalysisTaskResult result{.job_token = job_token};
        try {
            const BackendPeopleAnalysisExecution execution = execute(job_token, true);
            result.job_token = execution.job_token;
            result.made_progress = execution.made_progress;
            result.report = execution.report;
            result.cancelled = execution.cancelled;
            result.diagnostic = execution.diagnostic;
        } catch (const std::exception& error) {
            result.diagnostic = QString::fromUtf8(error.what());
        }
        return result;
    }));
    progress_timer_.start();
    emit stateChanged();
}

void PeopleAnalysisController::cancelAnalysis() {
    if (continuation_queued_) {
        continuation_queued_ = false;
        state_ = State::Cancelled;
        emit stateChanged();
    }
    if (!active_job_token_.has_value() || cancelRequested()) {
        return;
    }
    try {
        if (operations_.cancel(*active_job_token_)) {
            progress_.cancellation_requested = true;
            state_ = State::Cancelling;
            emit stateChanged();
        }
    } catch (const std::exception& error) {
        qWarning().noquote() << "People analysis cancellation failed:" << error.what();
    }
}

void PeopleAnalysisController::clearPeopleData() {
    if (busy()) {
        return;
    }
    try {
        operations_.clear();
    } catch (const std::exception& error) {
        qWarning().noquote() << "Local people data could not be cleared:" << error.what();
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    report_ = {};
    resetSelection();
    has_results_ = false;
    state_ = State::Idle;
    emit resultsChanged();
    emit stateChanged();
}

void PeopleAnalysisController::toggleGroupSelection(const QString& group_id) {
    if (busy() || !has_results_) {
        return;
    }
    const bool exists = std::any_of(
        report_.groups.cbegin(),
        report_.groups.cend(),
        [&group_id](const BackendPeopleGroup& group) { return group.group_id == group_id; }
    );
    if (!exists) {
        return;
    }
    if (selected_group_ids_.contains(group_id)) {
        selected_group_ids_.removeAll(group_id);
    } else {
        selected_group_ids_.push_back(group_id);
    }
    emit resultsChanged();
}

void PeopleAnalysisController::mergeSelectedGroups() {
    if (!canMergeSelectedGroups()) {
        return;
    }
    try {
        report_ = operations_.merge(selected_group_ids_);
    } catch (const std::exception& error) {
        qWarning().noquote() << "People merge could not be saved:" << error.what();
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    resetSelection();
    emit resultsChanged();
}

void PeopleAnalysisController::renameGroup(const QString& group_id, const QString& display_name) {
    if (busy() || !has_results_) {
        return;
    }
    const bool exists = std::any_of(
        report_.groups.cbegin(),
        report_.groups.cend(),
        [&group_id](const BackendPeopleGroup& group) { return group.group_id == group_id; }
    );
    if (!exists) {
        return;
    }
    try {
        report_ = operations_.rename(group_id, display_name);
    } catch (const std::exception& error) {
        qWarning().noquote() << "Person name could not be saved:" << error.what();
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    resetSelection();
    emit resultsChanged();
}

void PeopleAnalysisController::undoLastMerge() {
    if (busy() || !report_.can_undo_merge) {
        return;
    }
    try {
        report_ = operations_.undo_merge();
    } catch (const std::exception& error) {
        qWarning().noquote() << "People merge could not be undone:" << error.what();
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    resetSelection();
    emit resultsChanged();
}

QStringList PeopleAnalysisController::groupPhotoIds(const QString& id) const {
    for (const auto& group : report_.groups)
        if (group.group_id == id)
            return group.photo_ids;
    return {};
}
QString PeopleAnalysisController::groupName(const QString& id) const {
    for (qsizetype i = 0; i < report_.groups.size(); ++i) {
        const auto& group = report_.groups.at(i);
        if (group.group_id == id)
            return group.display_name.isEmpty() ? tr("Person %1").arg(i + 1) : group.display_name;
    }
    return {};
}
void PeopleAnalysisController::openGroup(const QString& id) {
    if (!groupPhotoIds(id).isEmpty())
        emit groupOpened(id, groupName(id), groupPhotoIds(id));
}
void PeopleAnalysisController::splitGroupPhotos(const QString& id, const QStringList& photo_ids) {
    if (busy() || !operations_.split)
        return;
    try {
        report_ = operations_.split(id, photo_ids);
        resetSelection();
        state_ = State::Ready;
        emit resultsChanged();
        emit stateChanged();
    } catch (const std::exception& error) {
        qWarning().noquote() << "People correction could not be saved:" << error.what();
        state_ = State::Failed;
        emit stateChanged();
    }
}

void PeopleAnalysisController::retranslateUi() {
    emit stateChanged();
    emit resultsChanged();
}

void PeopleAnalysisController::pollProgress() {
    if (!active_job_token_.has_value() || !watcher_.isRunning()) {
        return;
    }
    try {
        const BackendPeopleAnalysisProgress next = operations_.progress(*active_job_token_);
        if (next.job_token != *active_job_token_) {
            return;
        }
        progress_ = next;
        if (next.cancellation_requested) {
            state_ = State::Cancelling;
        }
        emit stateChanged();
    } catch (const std::exception& error) {
        qWarning().noquote() << "People analysis progress unavailable:" << error.what();
    }
}

void PeopleAnalysisController::retireJob(const std::uint64_t job_token) noexcept {
    try {
        operations_.retire(job_token);
    } catch (const std::exception& error) {
        qWarning().noquote() << "People analysis retirement failed:" << error.what();
    }
}

bool PeopleAnalysisController::selectedGroupsConflict() const noexcept {
    QSet<QString> photo_ids;
    for (const BackendPeopleGroup& group : report_.groups) {
        if (!selected_group_ids_.contains(group.group_id)) {
            continue;
        }
        for (const QString& photo_id : group.photo_ids) {
            if (photo_ids.contains(photo_id)) {
                return true;
            }
            photo_ids.insert(photo_id);
        }
    }
    return false;
}

void PeopleAnalysisController::resetSelection() {
    selected_group_ids_.clear();
}

void PeopleAnalysisController::finishAnalysis() {
    progress_timer_.stop();
    const PeopleAnalysisTaskResult result = watcher_.result();
    if (!active_job_token_.has_value() || result.job_token != *active_job_token_) {
        qWarning() << "Ignoring stale people analysis result" << result.job_token;
        return;
    }
    const std::uint64_t job_token = *active_job_token_;
    active_job_token_.reset();
    retireJob(job_token);
    if (result.cancelled || state_ == State::Cancelling) {
        try {
            report_ = operations_.load();
            has_results_ = report_.has_data;
        } catch (const std::exception&) { /* Keep the last complete projection. */
        }
        emit resultsChanged();
        state_ = State::Cancelled;
        emit stateChanged();
        return;
    }
    if (!result.diagnostic.isEmpty()) {
        qWarning().noquote() << "People analysis failed:" << result.diagnostic;
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    resetSelection();
    const bool advanced = result.made_progress;
    report_ = result.report;
    has_results_ = report_.has_data;
    state_ = State::Ready;
    if (report_.truncated && advanced && preferences_->peopleAnalysisExecutionAllowed()) {
        continuation_queued_ = true;
        QTimer::singleShot(0, this, [this] {
            if (!continuation_queued_)
                return;
            continuation_queued_ = false;
            startAnalysis();
        });
    }
    emit resultsChanged();
    emit stateChanged();
}
