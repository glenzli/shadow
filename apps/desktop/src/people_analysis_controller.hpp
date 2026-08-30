#pragma once

#include "backend/people_analysis_types.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <optional>

struct PeopleAnalysisTaskResult final {
    std::uint64_t job_token = 0;
    BackendPeopleAnalysisReport report;
    bool cancelled = false;
    QString diagnostic;
};

/// Owns the complete session-only people-analysis lifecycle exposed to QML.
///
/// Analysis starts only from an explicit user action, runs away from the UI
/// thread, retains only anonymous group membership needed for session merge
/// safety, and discards all results when this controller is destroyed or the
/// user clears the session.
class PeopleAnalysisController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool cancelRequested READ cancelRequested NOTIFY stateChanged)
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
    Q_PROPERTY(int selectedGroupCount READ selectedGroupCount NOTIFY resultsChanged)
    Q_PROPERTY(bool canMergeSelectedGroups READ canMergeSelectedGroups NOTIFY resultsChanged)
    Q_PROPERTY(bool canUndoMerge READ canUndoMerge NOTIFY resultsChanged)
    Q_PROPERTY(QString mergeSelectionText READ mergeSelectionText NOTIFY resultsChanged)

  public:
    struct Operations final {
        std::function<std::uint64_t()> begin;
        std::function<BackendPeopleAnalysisExecution(std::uint64_t)> execute;
        std::function<BackendPeopleAnalysisProgress(std::uint64_t)> progress;
        std::function<bool(std::uint64_t)> cancel;
        std::function<void(std::uint64_t)> retire;
    };

    explicit PeopleAnalysisController(Operations operations, QObject* parent = nullptr);
    ~PeopleAnalysisController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool cancelRequested() const noexcept;
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
    [[nodiscard]] int selectedGroupCount() const noexcept;
    [[nodiscard]] bool canMergeSelectedGroups() const noexcept;
    [[nodiscard]] bool canUndoMerge() const noexcept;
    [[nodiscard]] QString mergeSelectionText() const;

    Q_INVOKABLE void startAnalysis();
    Q_INVOKABLE void cancelAnalysis();
    Q_INVOKABLE void clearSessionResults();
    Q_INVOKABLE void toggleGroupSelection(const QString& group_id);
    Q_INVOKABLE void mergeSelectedGroups();
    Q_INVOKABLE void undoLastMerge();
    Q_INVOKABLE void retranslateUi();

  signals:
    void stateChanged();
    void resultsChanged();

  private:
    enum class State {
        Idle,
        Running,
        Cancelling,
        Cancelled,
        Ready,
        Failed,
    };

    [[nodiscard]] bool selectedGroupsConflict() const noexcept;
    void resetMergeState();
    void pollProgress();
    void retireJob(std::uint64_t job_token) noexcept;
    void finishAnalysis();

    Operations operations_;
    QFutureWatcher<PeopleAnalysisTaskResult> watcher_;
    QTimer progress_timer_;
    BackendPeopleAnalysisReport report_;
    BackendPeopleAnalysisProgress progress_;
    QStringList selected_group_ids_;
    QVector<BackendPeopleGroup> merge_undo_groups_;
    State state_ = State::Idle;
    bool has_results_ = false;
    bool has_merge_undo_ = false;
    qulonglong next_merged_group_id_ = 1;
    std::optional<std::uint64_t> active_job_token_;
};
