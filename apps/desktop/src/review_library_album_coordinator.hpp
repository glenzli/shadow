#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <functional>

/// Owns Review's complete Library album lifecycle.
///
/// The coordinator keeps selection and the authoritative album snapshot
/// together, serializes refresh and mutation workers, coalesces refreshes,
/// invalidates a deleted selection, publishes localized terminal status, and
/// waits for the active worker before destruction. ReviewController remains
/// only the stable QML facade and supplies the current smart-album query.
class ReviewLibraryAlbumCoordinator final : public QObject {
    Q_OBJECT

public:
    struct Operations final {
        std::function<QVector<BackendLibraryAlbum>()> albums;
        std::function<void(const QString& name)> create_manual;
        std::function<void(
            const QString& name,
            const BackendLibraryPhotoFilter& query
        )> create_smart;
        std::function<void(const QString& album_id, const QString& name)> rename;
        std::function<void(const QString& album_id)> remove;
        std::function<void(const QString& album_id, const QString& photo_id)>
            add_photo;
        std::function<void(const QString& album_id, const QString& photo_id)>
            remove_photo;
    };

    enum class StatusChannel {
        None,
        Decision,
        Global,
    };

    explicit ReviewLibraryAlbumCoordinator(
        Operations operations,
        QObject* parent = nullptr
    );
    ~ReviewLibraryAlbumCoordinator() override;

    [[nodiscard]] QString albumId() const;
    [[nodiscard]] QVariantList albums() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] StatusChannel statusChannel() const noexcept;
    [[nodiscard]] LocalizedUiMessage statusMessage() const;

    void setAlbumId(const QString& album_id);
    void clearAlbumSelection();
    void refresh();
    void createManual(const QString& name);
    void createSmart(
        const QString& name,
        BackendLibraryPhotoFilter query
    );
    void rename(const QString& album_id, const QString& name);
    void remove(const QString& album_id);
    void addPhotos(const QString& album_id, const QVariantList& targets);
    void removePhotos(const QString& album_id, const QVariantList& targets);
    void retranslateUi();

signals:
    void albumsChanged();
    void albumSelectionChanged();
    void queryChanged();
    void statusMessageChanged();

private:
    enum class TaskAction : std::uint8_t {
        Refresh,
        CreateManual,
        CreateSmart,
        Rename,
        Delete,
        AddPhotos,
        RemovePhotos,
    };

    struct TaskResult final {
        QVector<BackendLibraryAlbum> albums;
        QString error;
        quint64 request_id = 0;
        TaskAction action = TaskAction::Refresh;
        QString album_id;
        int affected_photo_count = 0;
        bool has_album_snapshot = false;
    };

    [[nodiscard]] static QStringList photoIds(const QVariantList& targets);
    [[nodiscard]] static TaskResult runTask(
        Operations operations,
        TaskAction action,
        QString name,
        QString album_id,
        QStringList photo_ids,
        BackendLibraryPhotoFilter smart_query,
        quint64 request_id
    );

    void startTask(
        TaskAction action,
        const QString& name = {},
        const QString& album_id = {},
        const QStringList& photo_ids = {},
        BackendLibraryPhotoFilter smart_query = {}
    );
    void finishTask();
    void publishSuccess(const TaskResult& result);
    void setStatus(StatusChannel channel, LocalizedUiMessage status);

    Operations operations_;
    bool task_running_ = false;
    bool refresh_pending_ = false;
    quint64 request_id_ = 0;
    quint64 active_request_id_ = 0;
    QString album_id_;
    QVector<BackendLibraryAlbum> albums_;
    StatusChannel status_channel_ = StatusChannel::None;
    LocalizedUiMessage status_message_;
    QFutureWatcher<TaskResult> watcher_;
};
