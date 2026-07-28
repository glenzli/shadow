#pragma once

#include "desktop_backend.hpp"
#include "review_photo_inspection_session.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <memory>

struct ReviewPhotoInspectionTaskResult final {
    BackendPhotoInspection inspection;
    QString error;
    ReviewPhotoInspectionRequest request;
};

/// Owns the complete asynchronous lifecycle for exact selected-photo details.
///
/// Library paging and ReviewController never participate in this state
/// machine. Rapid reselection is coalesced to the newest exact identity, while
/// the session generation rejects stale completions.
class ReviewPhotoInspectionCoordinator final : public QObject {
    Q_OBJECT

public:
    using Loader = std::function<BackendPhotoInspection(
        const QString& photo_id,
        const QString& representation_id
    )>;

    explicit ReviewPhotoInspectionCoordinator(
        std::shared_ptr<DesktopBackend> backend,
        QObject* parent = nullptr
    );
    explicit ReviewPhotoInspectionCoordinator(
        Loader loader,
        QObject* parent = nullptr
    );
    ~ReviewPhotoInspectionCoordinator() override;

    [[nodiscard]] QVariantMap presentation() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool failed() const noexcept;

    void request(
        const QString& photo_id,
        const QString& representation_id
    );
    void retry();
    void clear();

signals:
    void stateChanged();

private:
    void start();
    void finish();

    Loader loader_;
    ReviewPhotoInspectionSession session_;
    BackendPhotoInspection inspection_;
    QString error_;
    bool running_ = false;
    bool pending_ = false;
    QFutureWatcher<ReviewPhotoInspectionTaskResult> watcher_;
};
