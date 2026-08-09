#pragma once

#include "backend/people_analysis_types.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QVariantList>

#include <functional>

struct PeopleAnalysisTaskResult final {
    BackendPeopleAnalysisReport report;
    QString diagnostic;
};

/// Owns the complete session-only people-analysis lifecycle exposed to QML.
///
/// Analysis starts only from an explicit user action, runs away from the UI
/// thread, retains only anonymous counts, and discards all results when this
/// controller is destroyed or the user clears the session.
class PeopleAnalysisController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool hasResults READ hasResults NOTIFY resultsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY resultsChanged)
    Q_PROPERTY(uint analyzedPhotos READ analyzedPhotos NOTIFY resultsChanged)
    Q_PROPERTY(uint detectedFaces READ detectedFaces NOTIFY resultsChanged)
    Q_PROPERTY(uint embeddedFaces READ embeddedFaces NOTIFY resultsChanged)
    Q_PROPERTY(uint skippedItems READ skippedItems NOTIFY resultsChanged)
    Q_PROPERTY(uint ungroupedFaces READ ungroupedFaces NOTIFY resultsChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY resultsChanged)

  public:
    using Runner = std::function<BackendPeopleAnalysisReport()>;

    explicit PeopleAnalysisController(Runner runner, QObject* parent = nullptr);
    ~PeopleAnalysisController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool hasResults() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QVariantList groups() const;
    [[nodiscard]] uint analyzedPhotos() const noexcept;
    [[nodiscard]] uint detectedFaces() const noexcept;
    [[nodiscard]] uint embeddedFaces() const noexcept;
    [[nodiscard]] uint skippedItems() const noexcept;
    [[nodiscard]] uint ungroupedFaces() const noexcept;
    [[nodiscard]] bool truncated() const noexcept;

    Q_INVOKABLE void startAnalysis();
    Q_INVOKABLE void clearSessionResults();
    Q_INVOKABLE void retranslateUi();

  signals:
    void stateChanged();
    void resultsChanged();

  private:
    enum class State {
        Idle,
        Running,
        Ready,
        Failed,
    };

    void finishAnalysis();

    Runner runner_;
    QFutureWatcher<PeopleAnalysisTaskResult> watcher_;
    BackendPeopleAnalysisReport report_;
    State state_ = State::Idle;
    bool has_results_ = false;
};
