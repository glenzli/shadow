#include "people_analysis_controller.hpp"

#include <QtConcurrentRun>

#include <QDebug>
#include <QSet>
#include <QVariantMap>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace {

[[nodiscard]] QString thumbnail_source(const QByteArray& jpeg) {
    if (jpeg.isEmpty()) {
        return {};
    }
    return QStringLiteral("data:image/jpeg;base64,%1").arg(QString::fromLatin1(jpeg.toBase64()));
}

} // namespace

PeopleAnalysisController::PeopleAnalysisController(Runner runner, QObject* const parent) :
    QObject(parent), runner_(std::move(runner)) {
    connect(
        &watcher_,
        &QFutureWatcher<PeopleAnalysisTaskResult>::finished,
        this,
        &PeopleAnalysisController::finishAnalysis
    );
}

PeopleAnalysisController::~PeopleAnalysisController() {
    watcher_.waitForFinished();
}

bool PeopleAnalysisController::busy() const noexcept {
    return watcher_.isRunning();
}

bool PeopleAnalysisController::hasResults() const noexcept {
    return has_results_;
}

QString PeopleAnalysisController::statusText() const {
    switch (state_) {
    case State::Idle:
        return tr("Ready to organize people locally.");
    case State::Running:
        return tr("Finding faces and preparing anonymous groups…");
    case State::Ready:
        return tr("Local people analysis finished.");
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
                {QStringLiteral("displayIndex"), index + 1},
                {QStringLiteral("photoCount"), group.member_count},
                {QStringLiteral("thumbnailSource"), thumbnail_source(group.thumbnail_jpeg)},
                {QStringLiteral("selected"), selected_group_ids_.contains(group.group_id)},
                {QStringLiteral("merged"),
                 group.group_id.startsWith(QStringLiteral("session-merged-person-"))},
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
    return selected_group_ids_.size() >= 2 && !selectedGroupsConflict();
}

bool PeopleAnalysisController::canUndoMerge() const noexcept {
    return has_merge_undo_;
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

void PeopleAnalysisController::startAnalysis() {
    if (watcher_.isRunning()) {
        return;
    }
    state_ = State::Running;
    emit stateChanged();
    watcher_.setFuture(QtConcurrent::run([runner = runner_]() {
        PeopleAnalysisTaskResult result;
        try {
            result.report = runner();
        } catch (const std::exception& error) {
            result.diagnostic = QString::fromUtf8(error.what());
        }
        return result;
    }));
    emit stateChanged();
}

void PeopleAnalysisController::clearSessionResults() {
    if (watcher_.isRunning()) {
        return;
    }
    report_ = {};
    resetMergeState();
    has_results_ = false;
    state_ = State::Idle;
    emit resultsChanged();
    emit stateChanged();
}

void PeopleAnalysisController::toggleGroupSelection(const QString& group_id) {
    if (watcher_.isRunning() || !has_results_) {
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

    QSet<QString> selected_ids;
    selected_ids.reserve(selected_group_ids_.size());
    for (const QString& group_id : std::as_const(selected_group_ids_)) {
        selected_ids.insert(group_id);
    }

    qsizetype first_selected_index = -1;
    std::uint64_t merged_member_count = 0;
    QSet<QString> merged_photo_ids;
    for (qsizetype index = 0; index < report_.groups.size(); ++index) {
        const BackendPeopleGroup& group = report_.groups.at(index);
        if (!selected_ids.contains(group.group_id)) {
            continue;
        }
        if (first_selected_index < 0) {
            first_selected_index = index;
        }
        merged_member_count += group.member_count;
        for (const QString& photo_id : group.photo_ids) {
            merged_photo_ids.insert(photo_id);
        }
    }
    if (first_selected_index < 0) {
        return;
    }

    merge_undo_groups_ = report_.groups;
    has_merge_undo_ = true;
    QStringList photo_ids;
    photo_ids.reserve(merged_photo_ids.size());
    for (const QString& photo_id : std::as_const(merged_photo_ids)) {
        photo_ids.push_back(photo_id);
    }
    photo_ids.sort();
    const auto bounded_member_count = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(merged_member_count, std::numeric_limits<std::uint32_t>::max())
    );
    BackendPeopleGroup merged_group{
        .group_id = QStringLiteral("session-merged-person-%1").arg(next_merged_group_id_++),
        .member_count = bounded_member_count,
        .photo_ids = std::move(photo_ids),
        .thumbnail_jpeg = report_.groups.at(first_selected_index).thumbnail_jpeg,
    };

    QVector<BackendPeopleGroup> merged_groups;
    merged_groups.reserve(report_.groups.size() - selected_ids.size() + 1);
    for (qsizetype index = 0; index < report_.groups.size(); ++index) {
        const BackendPeopleGroup& group = report_.groups.at(index);
        if (index == first_selected_index) {
            merged_groups.push_back(merged_group);
        }
        if (!selected_ids.contains(group.group_id)) {
            merged_groups.push_back(group);
        }
    }
    report_.groups = std::move(merged_groups);
    selected_group_ids_.clear();
    emit resultsChanged();
}

void PeopleAnalysisController::undoLastMerge() {
    if (watcher_.isRunning() || !has_merge_undo_) {
        return;
    }
    report_.groups = std::exchange(merge_undo_groups_, {});
    has_merge_undo_ = false;
    selected_group_ids_.clear();
    emit resultsChanged();
}

void PeopleAnalysisController::retranslateUi() {
    emit stateChanged();
    emit resultsChanged();
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

void PeopleAnalysisController::resetMergeState() {
    selected_group_ids_.clear();
    merge_undo_groups_.clear();
    has_merge_undo_ = false;
    next_merged_group_id_ = 1;
}

void PeopleAnalysisController::finishAnalysis() {
    const PeopleAnalysisTaskResult result = watcher_.result();
    if (!result.diagnostic.isEmpty()) {
        qWarning().noquote() << "People analysis failed:" << result.diagnostic;
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    resetMergeState();
    report_ = result.report;
    has_results_ = true;
    state_ = State::Ready;
    emit resultsChanged();
    emit stateChanged();
}
