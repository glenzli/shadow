#pragma once

#include "backend/library_types.hpp"
#include "library_reverse_geocoder.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

/// Lazily resolves Library GPS coordinates after the initial photo page is
/// visible. Catalog reads/writes run off the UI thread and exactly one native
/// provider request is active at a time.
class ReviewLibraryPlaceResolutionCoordinator final : public QObject {
    Q_OBJECT

  public:
    struct Operations final {
        std::function<QVector<BackendLibraryPlaceResolutionCandidate>(std::uint32_t limit)>
            candidates;
        std::function<BackendRecordLibraryPlaceResolutionStatus(
            const BackendLibraryPlaceResolutionResult& result
        )>
            record;
    };

    explicit ReviewLibraryPlaceResolutionCoordinator(
        Operations operations,
        std::unique_ptr<LibraryReverseGeocoder> provider,
        QObject* parent = nullptr
    );
    ~ReviewLibraryPlaceResolutionCoordinator() override;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint64_t recordedCount() const noexcept;

    /// Starts or resumes a session-local pass. Calls are coalesced while work
    /// is already active and unavailable platforms remain an intentional no-op.
    void start();

  signals:
    void stateChanged();
    void placesChanged();

  private:
    struct CandidateTaskResult final {
        QVector<BackendLibraryPlaceResolutionCandidate> candidates;
        QString error;
    };

    struct RecordTaskResult final {
        BackendRecordLibraryPlaceResolutionStatus status =
            BackendRecordLibraryPlaceResolutionStatus::CoordinatesNoLongerUsed;
        QString error;
    };

    [[nodiscard]] static CandidateTaskResult
    runCandidateTask(Operations operations, std::uint32_t limit);
    [[nodiscard]] static RecordTaskResult runRecordTask(
        Operations operations,
        BackendLibraryPlaceResolutionResult result
    );
    [[nodiscard]] static QString coordinateKey(
        const BackendLibraryPlaceResolutionCandidate& candidate
    );

    void requestCandidates();
    void finishCandidates();
    void finishGeocode(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        std::optional<BackendLibraryPlaceResolutionResult> result,
        const QString& error
    );
    void finishRecord();
    void scheduleNext();
    void stopPass();

    Operations operations_;
    std::unique_ptr<LibraryReverseGeocoder> provider_;
    QSet<QString> failed_coordinates_;
    QTimer next_timer_;
    QFutureWatcher<CandidateTaskResult> candidate_watcher_;
    QFutureWatcher<RecordTaskResult> record_watcher_;
    bool running_ = false;
    bool stopping_ = false;
    bool restart_requested_ = false;
    std::uint64_t recorded_count_ = 0;
};
