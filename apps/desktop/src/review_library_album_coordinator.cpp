#include "review_library_album_coordinator.hpp"

#include <QSet>
#include <QtConcurrentRun>

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage album_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

} // namespace

ReviewLibraryAlbumCoordinator::ReviewLibraryAlbumCoordinator(
    Operations operations,
    QObject* parent
)
    : QObject(parent),
      operations_(std::move(operations)) {
    if (!operations_.albums || !operations_.create_manual
        || !operations_.create_smart || !operations_.rename
        || !operations_.remove || !operations_.add_photo
        || !operations_.remove_photo) {
        throw std::invalid_argument(
            "complete Review Library album operations are required"
        );
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewLibraryAlbumCoordinator::finishTask
    );
}

ReviewLibraryAlbumCoordinator::~ReviewLibraryAlbumCoordinator() {
    watcher_.waitForFinished();
}

QString ReviewLibraryAlbumCoordinator::albumId() const {
    return album_id_;
}

QVariantList ReviewLibraryAlbumCoordinator::albums() const {
    QVariantList result;
    result.reserve(albums_.size());
    for (const auto& album : albums_) {
        result.push_back(QVariantMap{
            {QStringLiteral("id"), album.id},
            {QStringLiteral("name"), album.name},
            {
                QStringLiteral("kind"),
                album.kind == BackendLibraryAlbumKind::Smart
                    ? QStringLiteral("smart") : QStringLiteral("manual"),
            },
        });
    }
    return result;
}

bool ReviewLibraryAlbumCoordinator::busy() const noexcept {
    return task_running_;
}

ReviewLibraryAlbumCoordinator::StatusChannel
ReviewLibraryAlbumCoordinator::statusChannel() const noexcept {
    return status_channel_;
}

LocalizedUiMessage ReviewLibraryAlbumCoordinator::statusMessage() const {
    return status_message_;
}

void ReviewLibraryAlbumCoordinator::setAlbumId(const QString& album_id) {
    const QString normalized = album_id.trimmed();
    if (album_id_ == normalized) {
        return;
    }
    album_id_ = normalized;
    emit albumSelectionChanged();
    emit queryChanged();
}

void ReviewLibraryAlbumCoordinator::clearAlbumSelection() {
    setAlbumId({});
}

void ReviewLibraryAlbumCoordinator::refresh() {
    if (task_running_) {
        refresh_pending_ = true;
        return;
    }
    startTask(TaskAction::Refresh);
}

void ReviewLibraryAlbumCoordinator::createManual(const QString& name) {
    const QString normalized = name.trimmed();
    if (task_running_ || normalized.isEmpty()) {
        return;
    }
    startTask(TaskAction::CreateManual, normalized);
}

void ReviewLibraryAlbumCoordinator::createSmart(
    const QString& name,
    BackendLibraryPhotoFilter query
) {
    const QString normalized = name.trimmed();
    if (task_running_ || normalized.isEmpty()) {
        return;
    }
    query.album_id.clear();
    startTask(TaskAction::CreateSmart, normalized, {}, {}, std::move(query));
}

void ReviewLibraryAlbumCoordinator::rename(
    const QString& album_id,
    const QString& name
) {
    const QString normalized_album_id = album_id.trimmed();
    const QString normalized_name = name.trimmed();
    if (task_running_ || normalized_album_id.isEmpty()
        || normalized_name.isEmpty()) {
        return;
    }
    startTask(TaskAction::Rename, normalized_name, normalized_album_id);
}

void ReviewLibraryAlbumCoordinator::remove(const QString& album_id) {
    const QString normalized_album_id = album_id.trimmed();
    if (task_running_ || normalized_album_id.isEmpty()) {
        return;
    }
    startTask(TaskAction::Delete, {}, normalized_album_id);
}

void ReviewLibraryAlbumCoordinator::addPhotos(
    const QString& album_id,
    const QVariantList& targets
) {
    const QString normalized_album_id = album_id.trimmed();
    const QStringList photo_ids = photoIds(targets);
    if (task_running_ || normalized_album_id.isEmpty() || photo_ids.isEmpty()) {
        return;
    }
    startTask(TaskAction::AddPhotos, {}, normalized_album_id, photo_ids);
}

void ReviewLibraryAlbumCoordinator::removePhotos(
    const QString& album_id,
    const QVariantList& targets
) {
    const QString normalized_album_id = album_id.trimmed();
    const QStringList photo_ids = photoIds(targets);
    if (task_running_ || normalized_album_id.isEmpty() || photo_ids.isEmpty()) {
        return;
    }
    startTask(TaskAction::RemovePhotos, {}, normalized_album_id, photo_ids);
}

void ReviewLibraryAlbumCoordinator::retranslateUi() {
    if (!status_message_.isEmpty()) {
        emit statusMessageChanged();
    }
}

QStringList ReviewLibraryAlbumCoordinator::photoIds(
    const QVariantList& targets
) {
    QStringList photo_ids;
    QSet<QString> seen;
    for (const QVariant& value : targets) {
        const QString photo_id = value.toMap()
            .value(QStringLiteral("photoId"))
            .toString()
            .trimmed();
        if (!photo_id.isEmpty() && !seen.contains(photo_id)) {
            seen.insert(photo_id);
            photo_ids.push_back(photo_id);
        }
    }
    return photo_ids;
}

ReviewLibraryAlbumCoordinator::TaskResult
ReviewLibraryAlbumCoordinator::runTask(
    Operations operations,
    const TaskAction action,
    QString name,
    QString album_id,
    QStringList photo_ids,
    BackendLibraryPhotoFilter smart_query,
    const quint64 request_id
) {
    TaskResult result;
    result.request_id = request_id;
    result.action = action;
    result.album_id = album_id;
    result.affected_photo_count = static_cast<int>(photo_ids.size());
    try {
        switch (action) {
        case TaskAction::Refresh:
            break;
        case TaskAction::CreateManual:
            operations.create_manual(name);
            break;
        case TaskAction::CreateSmart:
            operations.create_smart(name, smart_query);
            break;
        case TaskAction::Rename:
            operations.rename(album_id, name);
            break;
        case TaskAction::Delete:
            operations.remove(album_id);
            break;
        case TaskAction::AddPhotos:
            for (const QString& photo_id : photo_ids) {
                operations.add_photo(album_id, photo_id);
            }
            break;
        case TaskAction::RemovePhotos:
            for (const QString& photo_id : photo_ids) {
                operations.remove_photo(album_id, photo_id);
            }
            break;
        }
        result.albums = operations.albums();
        result.has_album_snapshot = true;
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
        try {
            result.albums = operations.albums();
            result.has_album_snapshot = true;
        } catch (const std::exception&) {
            // Preserve the primary mutation or refresh failure.
        }
    }
    return result;
}

void ReviewLibraryAlbumCoordinator::startTask(
    const TaskAction action,
    const QString& name,
    const QString& album_id,
    const QStringList& photo_ids,
    BackendLibraryPhotoFilter smart_query
) {
    if (task_running_) {
        refresh_pending_ = true;
        return;
    }
    task_running_ = true;
    active_request_id_ = ++request_id_;
    if (action != TaskAction::Refresh) {
        setStatus(
            StatusChannel::Decision,
            album_message(QT_TRANSLATE_NOOP(
                "ReviewController",
                "Updating Library organization…"
            ))
        );
    }
    emit albumsChanged();
    watcher_.setFuture(QtConcurrent::run(
        runTask,
        operations_,
        action,
        name,
        album_id,
        photo_ids,
        std::move(smart_query),
        active_request_id_
    ));
}

void ReviewLibraryAlbumCoordinator::finishTask() {
    TaskResult result = watcher_.result();
    task_running_ = false;
    const bool accepted = result.request_id == active_request_id_;
    bool query_changed = false;
    if (accepted && result.has_album_snapshot) {
        albums_ = std::move(result.albums);
        if (!album_id_.isEmpty()) {
            const auto selected = std::find_if(
                albums_.cbegin(),
                albums_.cend(),
                [this](const BackendLibraryAlbum& album) {
                    return album.id == album_id_;
                }
            );
            if (selected == albums_.cend()) {
                album_id_.clear();
                query_changed = true;
                emit albumSelectionChanged();
            }
        }
    }

    if (accepted && result.error.isEmpty()) {
        if ((result.action == TaskAction::AddPhotos
                || result.action == TaskAction::RemovePhotos)
            && result.album_id == album_id_) {
            query_changed = true;
        }
        publishSuccess(result);
    } else if (accepted) {
        setStatus(
            StatusChannel::Global,
            album_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "Could not update Library albums · %1"
                ),
                {result.error}
            )
        );
    }

    emit albumsChanged();
    if (query_changed) {
        emit queryChanged();
    }
    if (refresh_pending_ || !accepted) {
        refresh_pending_ = false;
        startTask(TaskAction::Refresh);
    }
}

void ReviewLibraryAlbumCoordinator::publishSuccess(const TaskResult& result) {
    switch (result.action) {
    case TaskAction::Refresh:
        return;
    case TaskAction::CreateManual:
    case TaskAction::CreateSmart:
        setStatus(
            StatusChannel::Decision,
            album_message(QT_TRANSLATE_NOOP(
                "ReviewController",
                "Library album created"
            ))
        );
        return;
    case TaskAction::Rename:
        setStatus(
            StatusChannel::Decision,
            album_message(QT_TRANSLATE_NOOP(
                "ReviewController",
                "Library album renamed"
            ))
        );
        return;
    case TaskAction::Delete:
        setStatus(
            StatusChannel::Decision,
            album_message(QT_TRANSLATE_NOOP(
                "ReviewController",
                "Library album deleted"
            ))
        );
        return;
    case TaskAction::AddPhotos:
        setStatus(
            StatusChannel::Decision,
            album_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "%1 photos added to the album"
                ),
                {QString::number(result.affected_photo_count)}
            )
        );
        return;
    case TaskAction::RemovePhotos:
        setStatus(
            StatusChannel::Decision,
            album_message(
                QT_TRANSLATE_NOOP(
                    "ReviewController",
                    "%1 photos removed from the album"
                ),
                {QString::number(result.affected_photo_count)}
            )
        );
        return;
    }
}

void ReviewLibraryAlbumCoordinator::setStatus(
    const StatusChannel channel,
    LocalizedUiMessage status
) {
    status_channel_ = channel;
    status_message_ = std::move(status);
    emit statusMessageChanged();
}
