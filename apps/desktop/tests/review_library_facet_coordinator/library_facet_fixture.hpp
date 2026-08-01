#pragma once

#include "review_library_facet_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace review_library_facet_test {

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Library facet coordinator contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate> void wait_until(Predicate predicate, const std::string& message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

struct FacetCall final {
    BackendLibraryFacetKind kind = BackendLibraryFacetKind::CaptureMonth;
    QString camera_key;
    QString album_id;
    QString cursor_key;
    std::uint32_t limit = 0;
};

struct CountCall final {
    bool has_liked = false;
    bool liked = false;
    bool has_minimum_rating = false;
    std::uint8_t minimum_rating = 0;
    QString album_id;
};

struct FacetBackendState final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    QVector<FacetCall> calls;
    QVector<CountCall> count_calls;
    bool block_first = false;
    bool first_entered = false;
    bool release_first = false;
    bool fail = false;
};

inline QString kind_name(const BackendLibraryFacetKind kind) {
    switch (kind) {
    case BackendLibraryFacetKind::CaptureMonth:
        return QStringLiteral("month");
    case BackendLibraryFacetKind::Camera:
        return QStringLiteral("camera");
    case BackendLibraryFacetKind::Lens:
        return QStringLiteral("lens");
    case BackendLibraryFacetKind::Country:
        return QStringLiteral("country");
    case BackendLibraryFacetKind::City:
        return QStringLiteral("city");
    }
    return QStringLiteral("unknown");
}

inline ReviewLibraryFacetCoordinator::Operations
operations(const std::shared_ptr<FacetBackendState>& state) {
    return {
        .page =
            [state](
                const BackendLibraryPhotoFilter& filter,
                const BackendLibraryFacetKind kind,
                const BackendLibraryFacetCursor& cursor,
                const std::uint32_t limit
            ) {
                std::unique_lock lock(state->mutex);
                state->calls.push_back({
                    .kind = kind,
                    .camera_key = filter.camera_key,
                    .album_id = filter.album_id,
                    .cursor_key = cursor.key,
                    .limit = limit,
                });
                if (state->block_first && state->calls.size() == 1) {
                    state->first_entered = true;
                    state->condition.notify_all();
                    state->condition.wait(lock, [state]() { return state->release_first; });
                }
                if (state->fail) {
                    throw std::runtime_error("facet query failed");
                }
                const QString identity = filter.camera_key + QLatin1Char('-') + kind_name(kind);
                return BackendLibraryFacetPage{
                    .items = {
                        {
                            .key = identity,
                            .label = identity + QStringLiteral("-label"),
                            .photo_count = 7,
                        },
                    },
                };
            },
        .count =
            [state](const BackendLibraryPhotoFilter& filter) {
                std::lock_guard lock(state->mutex);
                state->count_calls.push_back({
                    .has_liked = filter.has_liked,
                    .liked = filter.liked,
                    .has_minimum_rating = filter.has_minimum_rating,
                    .minimum_rating = filter.minimum_rating,
                    .album_id = filter.album_id,
                });
                if (state->fail) {
                    throw std::runtime_error("facet query failed");
                }
                if (filter.has_liked && filter.liked) {
                    return std::uint64_t{7};
                }
                if (filter.has_minimum_rating && filter.minimum_rating == 5) {
                    return std::uint64_t{3};
                }
                return std::uint64_t{41};
            },
    };
}

inline int call_count(const std::shared_ptr<FacetBackendState>& state) {
    std::lock_guard lock(state->mutex);
    return static_cast<int>(state->calls.size() + state->count_calls.size());
}

} // namespace review_library_facet_test
