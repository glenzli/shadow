#pragma once

#include "review_library_query_coordinator.hpp"

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

namespace review_library_query_test {

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Library query coordinator contract failed: " << message << '\n';
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

struct PageCall final {
    QString camera_key;
    BackendLibraryPhotoOrder order = BackendLibraryPhotoOrder::CaptureTimeDescending;
    QString cursor_photo_id;
    std::uint32_t limit = 0;
};

struct QueryBackendState final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    QVector<PageCall> page_calls;
    QVector<QString> count_calls;
    bool block_first_page = false;
    bool first_page_entered = false;
    bool release_first_page = false;
    bool fail_page = false;
    bool fail_count = false;
    bool invalid_cursor = false;
};

inline BackendReviewItem item(
    const QString& photo_id,
    const QString& title,
    const std::uint64_t head = 1,
    const BackendReviewDecisionFlag flag = BackendReviewDecisionFlag::Unflagged,
    const std::uint8_t rating = 0
) {
    return {
        .photo_id = photo_id,
        .representation_id = photo_id + QStringLiteral("-representation"),
        .location_id = photo_id + QStringLiteral("-location"),
        .visual_handle = photo_id + QStringLiteral("-visual"),
        .decision_head_sequence = head,
        .decision_flag = flag,
        .decision_rating = rating,
        .title = title,
        .source_path = QStringLiteral("/tmp/") + photo_id,
        .source_available = false,
    };
}

inline ReviewLibraryQueryCoordinator::Operations
operations(const std::shared_ptr<QueryBackendState>& state) {
    return {
        .page =
            [state](
                const BackendLibraryPhotoFilter& filter,
                const BackendLibraryPhotoOrder order,
                const BackendLibraryPhotoCursor& cursor,
                const std::uint32_t limit
            ) {
                std::unique_lock lock(state->mutex);
                state->page_calls.push_back({
                    .camera_key = filter.camera_key,
                    .order = order,
                    .cursor_photo_id = cursor.photo_id,
                    .limit = limit,
                });
                if (state->block_first_page && state->page_calls.size() == 1) {
                    state->first_page_entered = true;
                    state->condition.notify_all();
                    state->condition.wait(lock, [state]() { return state->release_first_page; });
                }
                if (state->fail_page) {
                    throw std::runtime_error("page failed");
                }
                if (filter.camera_key == QStringLiteral("paginate")) {
                    if (cursor.photo_id.isEmpty()) {
                        return BackendLibraryPhotoPage{
                            .items =
                                {
                                    item(
                                        QStringLiteral("photo-1"),
                                        QStringLiteral("first"),
                                        4,
                                        BackendReviewDecisionFlag::Picked,
                                        3
                                    ),
                                },
                            .has_more = true,
                            .next_cursor = {
                                .photo_id =
                                    state->invalid_cursor ? QString{} : QStringLiteral("photo-1"),
                                .has_capture_time =
                                    order == BackendLibraryPhotoOrder::CaptureTimeDescending
                                    || order == BackendLibraryPhotoOrder::CaptureTimeAscending,
                                .captured_at_unix_seconds = 10,
                                .file_name =
                                    order == BackendLibraryPhotoOrder::FileNameAscending
                                            || order == BackendLibraryPhotoOrder::FileNameDescending
                                        ? QStringLiteral("first.nef")
                                        : QString{},
                            },
                        };
                    }
                    return BackendLibraryPhotoPage{
                        .items = {
                            item(QStringLiteral("photo-2"), QStringLiteral("second")),
                        },
                    };
                }
                const QString identity =
                    filter.camera_key.isEmpty() ? QStringLiteral("all") : filter.camera_key;
                return BackendLibraryPhotoPage{
                    .items = {
                        item(identity, identity + QStringLiteral("-title")),
                    },
                };
            },
        .count =
            [state](const BackendLibraryPhotoFilter& filter) {
                std::lock_guard lock(state->mutex);
                state->count_calls.push_back(filter.camera_key);
                if (state->fail_count) {
                    throw std::runtime_error("count failed");
                }
                return filter.camera_key == QStringLiteral("paginate") ? quint64{2} : quint64{1};
            },
    };
}

inline void wait_for_first_page(const std::shared_ptr<QueryBackendState>& state) {
    std::unique_lock lock(state->mutex);
    state->condition.wait(lock, [state]() { return state->first_page_entered; });
}

inline void release_first_page(const std::shared_ptr<QueryBackendState>& state) {
    {
        std::lock_guard lock(state->mutex);
        state->release_first_page = true;
    }
    state->condition.notify_all();
}

inline int page_call_count(const std::shared_ptr<QueryBackendState>& state) {
    std::lock_guard lock(state->mutex);
    return static_cast<int>(state->page_calls.size());
}

} // namespace review_library_query_test
