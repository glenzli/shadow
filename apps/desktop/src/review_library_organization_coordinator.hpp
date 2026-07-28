#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"

#include <QFutureWatcher>
#include <QObject>

#include <functional>
#include <optional>

/// Owns Review's complete per-photo Library organization mutation lifecycle.
///
/// Like and color label are persisted as one authoritative state. This owner
/// performs admission, resolves the currently loaded state, serializes the
/// worker, validates the backend receipt, projects it, publishes localized
/// status, and waits for the active mutation before destruction.
class ReviewLibraryOrganizationCoordinator final : public QObject {
    Q_OBJECT

public:
    struct CurrentState final {
        bool liked = false;
        QString color_label;
    };

    struct Operations final {
        std::function<std::optional<CurrentState>(const QString& photo_id)>
            current;
        std::function<BackendPhotoLibraryState(
            const QString& photo_id,
            bool liked,
            const QString& color_label
        )> mutate;
        std::function<bool(const BackendPhotoLibraryState& state)> project;
    };

    explicit ReviewLibraryOrganizationCoordinator(
        Operations operations,
        QObject* parent = nullptr
    );
    ~ReviewLibraryOrganizationCoordinator() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] LocalizedUiMessage statusMessage() const;

    void setColorLabel(
        const QString& photo_id,
        const QString& color_label,
        bool admitted
    );
    void setLiked(const QString& photo_id, bool liked, bool admitted);
    void retranslateUi();

signals:
    void stateProjected(
        const QString& photoId,
        bool liked,
        const QString& colorLabel
    );
    void statusMessageChanged();

private:
    struct TaskResult final {
        BackendPhotoLibraryState state;
        QString error;
        QString requested_photo_id;
    };

    [[nodiscard]] static TaskResult runTask(
        std::function<BackendPhotoLibraryState(
            const QString& photo_id,
            bool liked,
            const QString& color_label
        )> operation,
        QString photo_id,
        bool liked,
        QString color_label
    );

    void startTask(
        const QString& photo_id,
        bool liked,
        const QString& color_label
    );
    void finishTask();
    void setStatus(LocalizedUiMessage status);

    Operations operations_;
    bool task_running_ = false;
    LocalizedUiMessage status_message_;
    QFutureWatcher<TaskResult> watcher_;
};
