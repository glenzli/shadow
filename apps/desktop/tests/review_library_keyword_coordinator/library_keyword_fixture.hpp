#pragma once

#include "review_library_keyword_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QThread>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace review_library_keyword_test {

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Library keyword coordinator contract failed: " << message << '\n';
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

inline BackendLibraryKeyword keyword(
    const QString& id,
    const QString& name,
    const QString& parent_id = {},
    const std::uint16_t depth = 0,
    const std::uint64_t photo_count = 0
) {
    return {
        .id = id,
        .parent_id = parent_id,
        .name = name,
        .depth = depth,
        .subtree_photo_count = photo_count,
    };
}

inline BackendLibraryPhotoKeyword assignment(
    BackendLibraryKeyword value,
    const BackendLibraryKeywordOrigin origin = BackendLibraryKeywordOrigin::Manual
) {
    return {
        .keyword = std::move(value),
        .origin = origin,
    };
}

inline QVariant target(const QString& photo_id) {
    return QVariantMap{{QStringLiteral("photoId"), photo_id}};
}

struct KeywordBackendState final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    QVector<BackendLibraryKeyword> keywords;
    QHash<QString, QVector<BackendLibraryPhotoKeyword>> assignments;
    QStringList last_photo_ids;
    int next_id = 1;
    bool fail_create = false;
    bool block_tree = false;
    bool tree_entered = false;
    bool release_tree = false;
};

inline ReviewLibraryKeywordCoordinator::Operations
operations(const std::shared_ptr<KeywordBackendState>& state) {
    return {
        .tree =
            [state]() {
                std::unique_lock lock(state->mutex);
                if (state->block_tree) {
                    state->tree_entered = true;
                    state->condition.notify_all();
                    state->condition.wait(lock, [state]() { return state->release_tree; });
                }
                return state->keywords;
            },
        .for_photo =
            [state](const QString& photo_id) {
                std::lock_guard lock(state->mutex);
                return state->assignments.value(photo_id);
            },
        .create =
            [state](const QString& parent_id, const QString& name) {
                std::lock_guard lock(state->mutex);
                if (state->fail_create) {
                    throw std::runtime_error("create failed");
                }
                const QString id = QStringLiteral("keyword-%1").arg(state->next_id++);
                state->keywords.push_back(keyword(id, name, parent_id));
            },
        .rename =
            [state](const QString& keyword_id, const QString& name) {
                std::lock_guard lock(state->mutex);
                for (auto& value : state->keywords) {
                    if (value.id == keyword_id) {
                        value.name = name;
                    }
                }
            },
        .move =
            [state](const QString& keyword_id, const QString& parent_id) {
                std::lock_guard lock(state->mutex);
                for (auto& value : state->keywords) {
                    if (value.id == keyword_id) {
                        value.parent_id = parent_id;
                    }
                }
            },
        .remove =
            [state](const QString& keyword_id) {
                std::lock_guard lock(state->mutex);
                const auto before = state->keywords.size();
                state->keywords.erase(
                    std::remove_if(
                        state->keywords.begin(),
                        state->keywords.end(),
                        [&keyword_id](const BackendLibraryKeyword& value) {
                            return value.id == keyword_id;
                        }
                    ),
                    state->keywords.end()
                );
                return BackendLibraryKeywordDeletionReceipt{
                    .deleted_keyword_count =
                        static_cast<std::uint64_t>(before - state->keywords.size()),
                };
            },
        .assign =
            [state](const QString& keyword_id, const QStringList& photo_ids) {
                std::lock_guard lock(state->mutex);
                state->last_photo_ids = photo_ids;
                const auto found = std::find_if(
                    state->keywords.cbegin(),
                    state->keywords.cend(),
                    [&keyword_id](const BackendLibraryKeyword& value) {
                        return value.id == keyword_id;
                    }
                );
                if (found != state->keywords.cend()) {
                    for (const QString& photo_id : photo_ids) {
                        state->assignments[photo_id] = {
                            assignment(*found),
                        };
                    }
                }
                return BackendLibraryKeywordMutationReceipt{
                    .keyword_id = keyword_id,
                    .requested_photo_count = static_cast<std::uint64_t>(photo_ids.size()),
                    .changed_photo_count = static_cast<std::uint64_t>(photo_ids.size()),
                };
            },
        .unassign =
            [state](const QString& keyword_id, const QStringList& photo_ids) {
                std::lock_guard lock(state->mutex);
                state->last_photo_ids = photo_ids;
                for (const QString& photo_id : photo_ids) {
                    state->assignments.remove(photo_id);
                }
                return BackendLibraryKeywordMutationReceipt{
                    .keyword_id = keyword_id,
                    .requested_photo_count = static_cast<std::uint64_t>(photo_ids.size()),
                    .changed_photo_count = static_cast<std::uint64_t>(photo_ids.size()),
                };
            },
    };
}

} // namespace review_library_keyword_test
