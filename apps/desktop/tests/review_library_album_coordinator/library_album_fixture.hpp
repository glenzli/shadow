#pragma once

#include "review_library_album_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace review_library_album_test {

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr
            << "Library album coordinator contract failed: "
            << message
            << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate>
void wait_until(Predicate predicate, const std::string& message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

inline BackendLibraryAlbum album(
    const QString& id,
    const QString& name,
    const BackendLibraryAlbumKind kind = BackendLibraryAlbumKind::Manual
) {
    return {
        .id = id,
        .kind = kind,
        .name = name,
    };
}

inline QVariant target(const QString& photo_id) {
    return QVariantMap{{QStringLiteral("photoId"), photo_id}};
}

struct AlbumBackendState final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    QVector<BackendLibraryAlbum> albums;
    QStringList added;
    QStringList removed;
    BackendLibraryPhotoFilter smart_query;
    int list_calls = 0;
    int next_album_id = 1;
    bool block_list = false;
    bool list_entered = false;
    bool release_list = false;
    bool fail_create = false;
};

inline ReviewLibraryAlbumCoordinator::Operations operations(
    const std::shared_ptr<AlbumBackendState>& state
) {
    return {
        .albums =
            [state]() {
                std::unique_lock lock(state->mutex);
                ++state->list_calls;
                if (state->block_list) {
                    state->list_entered = true;
                    state->condition.notify_all();
                    state->condition.wait(
                        lock,
                        [state]() { return state->release_list; }
                    );
                }
                return state->albums;
            },
        .create_manual =
            [state](const QString& name) {
                std::lock_guard lock(state->mutex);
                if (state->fail_create) {
                    throw std::runtime_error("create failed");
                }
                const QString id = QStringLiteral("manual-%1")
                    .arg(state->next_album_id++);
                state->albums.push_back(album(id, name));
            },
        .create_smart =
            [state](
                const QString& name,
                const BackendLibraryPhotoFilter& query
            ) {
                std::lock_guard lock(state->mutex);
                if (state->fail_create) {
                    throw std::runtime_error("create failed");
                }
                state->smart_query = query;
                const QString id = QStringLiteral("smart-%1")
                    .arg(state->next_album_id++);
                state->albums.push_back(
                    album(id, name, BackendLibraryAlbumKind::Smart)
                );
            },
        .rename =
            [state](const QString& album_id, const QString& name) {
                std::lock_guard lock(state->mutex);
                for (auto& candidate : state->albums) {
                    if (candidate.id == album_id) {
                        candidate.name = name;
                    }
                }
            },
        .remove =
            [state](const QString& album_id) {
                std::lock_guard lock(state->mutex);
                state->albums.erase(
                    std::remove_if(
                        state->albums.begin(),
                        state->albums.end(),
                        [&album_id](const BackendLibraryAlbum& candidate) {
                            return candidate.id == album_id;
                        }
                    ),
                    state->albums.end()
                );
            },
        .add_photo =
            [state](const QString& album_id, const QString& photo_id) {
                std::lock_guard lock(state->mutex);
                state->added.push_back(album_id + QLatin1Char(':') + photo_id);
            },
        .remove_photo =
            [state](const QString& album_id, const QString& photo_id) {
                std::lock_guard lock(state->mutex);
                state->removed.push_back(
                    album_id + QLatin1Char(':') + photo_id
                );
            },
    };
}

inline int list_calls(const std::shared_ptr<AlbumBackendState>& state) {
    std::lock_guard lock(state->mutex);
    return state->list_calls;
}

} // namespace review_library_album_test
