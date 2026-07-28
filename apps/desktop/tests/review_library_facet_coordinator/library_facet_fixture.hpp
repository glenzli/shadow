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
        std::cerr
            << "Library facet coordinator contract failed: "
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

struct FacetCall final {
    BackendLibraryFacetKind kind = BackendLibraryFacetKind::CaptureMonth;
    QString camera_key;
    QString album_id;
    QString cursor_key;
    std::uint32_t limit = 0;
};

struct FacetBackendState final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    QVector<FacetCall> calls;
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
    }
    return QStringLiteral("unknown");
}

inline ReviewLibraryFacetCoordinator::Operations operations(
    const std::shared_ptr<FacetBackendState>& state
) {
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
                    state->condition.wait(
                        lock,
                        [state]() { return state->release_first; }
                    );
                }
                if (state->fail) {
                    throw std::runtime_error("facet query failed");
                }
                const QString identity =
                    filter.camera_key + QLatin1Char('-') + kind_name(kind);
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
    };
}

inline int call_count(const std::shared_ptr<FacetBackendState>& state) {
    std::lock_guard lock(state->mutex);
    return static_cast<int>(state->calls.size());
}

} // namespace review_library_facet_test
