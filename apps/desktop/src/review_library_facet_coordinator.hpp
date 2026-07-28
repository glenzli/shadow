#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QVariantList>

#include <cstdint>
#include <functional>

/// Owns Review's generation-bound Library facet projection.
///
/// One worker fetches the three independently bounded facet dimensions from
/// the same immutable filter snapshot. The coordinator rejects stale
/// generations, coalesces refreshes onto the latest input, publishes the
/// stable QML projection and localized failure, and waits on destruction.
class ReviewLibraryFacetCoordinator final : public QObject {
    Q_OBJECT

public:
    struct Operations final {
        std::function<BackendLibraryFacetPage(
            const BackendLibraryPhotoFilter& filter,
            BackendLibraryFacetKind kind,
            const BackendLibraryFacetCursor& cursor,
            std::uint32_t limit
        )> page;
    };

    explicit ReviewLibraryFacetCoordinator(
        Operations operations,
        QObject* parent = nullptr
    );
    ~ReviewLibraryFacetCoordinator() override;

    [[nodiscard]] QVariantList captureMonths() const;
    [[nodiscard]] QVariantList cameras() const;
    [[nodiscard]] QVariantList lenses() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] LocalizedUiMessage globalStatusMessage() const;

    void refresh(BackendLibraryPhotoFilter filter, quint64 library_generation);
    void retranslateUi();

signals:
    void facetsChanged();
    void globalStatusMessageChanged();

private:
    struct TaskResult final {
        BackendLibraryFacetPage capture_months;
        BackendLibraryFacetPage cameras;
        BackendLibraryFacetPage lenses;
        QString error;
        quint64 library_generation = 0;
        quint64 request_id = 0;
    };

    [[nodiscard]] static QVariantList variants(
        const BackendLibraryFacetPage& page
    );
    [[nodiscard]] static TaskResult runTask(
        Operations operations,
        BackendLibraryPhotoFilter filter,
        quint64 library_generation,
        quint64 request_id
    );

    void startTask();
    void finishTask();
    void publishGlobalStatus(LocalizedUiMessage status);

    Operations operations_;
    BackendLibraryPhotoFilter requested_filter_;
    quint64 requested_library_generation_ = 0;
    bool task_running_ = false;
    bool refresh_pending_ = false;
    quint64 request_id_ = 0;
    quint64 active_request_id_ = 0;
    BackendLibraryFacetPage capture_months_;
    BackendLibraryFacetPage cameras_;
    BackendLibraryFacetPage lenses_;
    LocalizedUiMessage global_status_message_;
    QFutureWatcher<TaskResult> watcher_;
};
