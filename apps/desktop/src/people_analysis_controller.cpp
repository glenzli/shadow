#include "people_analysis_controller.hpp"

#include <QtConcurrentRun>

#include <QDebug>
#include <QVariantMap>

#include <exception>
#include <utility>

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
    has_results_ = false;
    state_ = State::Idle;
    emit resultsChanged();
    emit stateChanged();
}

void PeopleAnalysisController::retranslateUi() {
    emit stateChanged();
}

void PeopleAnalysisController::finishAnalysis() {
    const PeopleAnalysisTaskResult result = watcher_.result();
    if (!result.diagnostic.isEmpty()) {
        qWarning().noquote() << "People analysis failed:" << result.diagnostic;
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    report_ = result.report;
    has_results_ = true;
    state_ = State::Ready;
    emit resultsChanged();
    emit stateChanged();
}
