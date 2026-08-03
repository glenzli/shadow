#pragma once

#include "desktop_backend.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QVariantList>

#include <cstdint>
#include <functional>

/// Owns the profile-driven Travel smart-collection projection.
///
/// One worker reads an unfiltered home-place candidate list and, when a home
/// locality exists, derives an outside-home count plus country/city hierarchy.
/// Requests are generation-bound and coalesced; no profile state is persisted
/// here and no current Library grid filter is mutated by projection work.
class ReviewTravelCollectionCoordinator final : public QObject {
    Q_OBJECT

  public:
    struct Operations final {
        std::function<BackendLibraryFacetPage(
            const BackendLibraryPhotoFilter& filter,
            BackendLibraryFacetKind kind,
            const BackendLibraryFacetCursor& cursor,
            std::uint32_t limit
        )>
            page;
        std::function<std::uint64_t(const BackendLibraryPhotoFilter& filter)> count;
    };

    explicit ReviewTravelCollectionCoordinator(Operations operations, QObject* parent = nullptr);
    ~ReviewTravelCollectionCoordinator() override;

    [[nodiscard]] QVariantList homeCandidates() const;
    [[nodiscard]] QVariantList groups() const;
    [[nodiscard]] qulonglong photoCount() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString errorText() const;

    void refresh(QString home_locality_key, quint64 library_generation);

  signals:
    void projectionChanged();

  private:
    struct CountryGroup final {
        BackendLibraryFacet country;
        BackendLibraryFacetPage destinations;
    };

    struct TaskResult final {
        BackendLibraryFacetPage home_candidates;
        QVector<CountryGroup> groups;
        std::uint64_t photo_count = 0;
        QString error;
        QString home_locality_key;
        quint64 library_generation = 0;
        quint64 request_id = 0;
    };

    [[nodiscard]] static TaskResult runTask(
        Operations operations,
        QString home_locality_key,
        quint64 library_generation,
        quint64 request_id
    );
    [[nodiscard]] static QVariantList facetVariants(const BackendLibraryFacetPage& page);
    void startTask();
    void finishTask();

    Operations operations_;
    QString requested_home_locality_key_;
    quint64 requested_library_generation_ = 0;
    quint64 request_id_ = 0;
    quint64 active_request_id_ = 0;
    bool task_running_ = false;
    bool refresh_pending_ = false;
    BackendLibraryFacetPage home_candidates_;
    QVector<CountryGroup> groups_;
    std::uint64_t photo_count_ = 0;
    QString error_text_;
    QFutureWatcher<TaskResult> watcher_;
};
