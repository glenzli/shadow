#pragma once

#include "review_library_organization_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QThread>

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace review_library_organization_test {

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr
            << "Library organization coordinator contract failed: "
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

struct MutationCall final {
    QString photo_id;
    bool liked = false;
    QString color_label;
};

struct OrganizationBackendState final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    QHash<QString, ReviewLibraryOrganizationCoordinator::CurrentState> current;
    QVector<MutationCall> mutations;
    QVector<BackendPhotoLibraryState> projections;
    bool fail = false;
    bool wrong_receipt = false;
    bool block = false;
    bool entered = false;
    bool release = false;
};

inline ReviewLibraryOrganizationCoordinator::Operations operations(
    const std::shared_ptr<OrganizationBackendState>& state
) {
    return {
        .current =
            [state](const QString& photo_id)
                -> std::optional<
                    ReviewLibraryOrganizationCoordinator::CurrentState
                > {
                std::lock_guard lock(state->mutex);
                const auto found = state->current.constFind(photo_id);
                if (found == state->current.cend()) {
                    return std::nullopt;
                }
                return *found;
            },
        .mutate =
            [state](
                const QString& photo_id,
                const bool liked,
                const QString& color_label
            ) {
                std::unique_lock lock(state->mutex);
                state->mutations.push_back({
                    .photo_id = photo_id,
                    .liked = liked,
                    .color_label = color_label,
                });
                if (state->block) {
                    state->entered = true;
                    state->condition.notify_all();
                    state->condition.wait(
                        lock,
                        [state]() { return state->release; }
                    );
                }
                if (state->fail) {
                    throw std::runtime_error("organization write failed");
                }
                return BackendPhotoLibraryState{
                    .photo_id = state->wrong_receipt
                        ? QStringLiteral("wrong-photo") : photo_id,
                    .liked = liked,
                    .color_label = color_label,
                    .updated_at_ms = 77,
                };
            },
        .project =
            [state](const BackendPhotoLibraryState& projected) {
                std::lock_guard lock(state->mutex);
                state->projections.push_back(projected);
                state->current.insert(
                    projected.photo_id,
                    {
                        .liked = projected.liked,
                        .color_label = projected.color_label,
                    }
                );
                return true;
            },
    };
}

inline int mutation_count(
    const std::shared_ptr<OrganizationBackendState>& state
) {
    std::lock_guard lock(state->mutex);
    return static_cast<int>(state->mutations.size());
}

} // namespace review_library_organization_test
