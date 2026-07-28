#pragma once

#include "review_decision_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace review_decision_test {

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr
            << "review decision coordinator contract failed: "
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

[[nodiscard]] inline BackendReviewDecisionState state(
    const QString& photo_id,
    const std::uint64_t sequence,
    const BackendReviewDecisionFlag flag,
    const std::uint8_t rating
) {
    return {
        .photo_id = photo_id,
        .head_sequence = sequence,
        .flag = flag,
        .rating = rating,
    };
}

[[nodiscard]] inline BackendReviewDecisionMutationReceipt receipt(
    const QString& event_id,
    const BackendReviewDecisionState& before,
    const BackendReviewDecisionState& after
) {
    return {
        .event_id = event_id,
        .sequence = after.head_sequence,
        .occurred_at_ms = 1,
        .before = before,
        .after = after,
    };
}

[[nodiscard]] inline ReviewDecisionCoordinator::Operations operations(
    std::function<BackendReviewDecisionMutationReceipt(
        const QString&,
        std::uint64_t,
        BackendReviewDecisionFlag,
        std::uint8_t
    )> mutate,
    std::function<BackendReviewDecisionState(const QString&)>
        authoritative_state = [](const QString& photo_id) {
            return state(
                photo_id,
                1,
                BackendReviewDecisionFlag::Unflagged,
                0
            );
        }
) {
    return {
        .mutate = std::move(mutate),
        .authoritative_state = std::move(authoritative_state),
    };
}

} // namespace review_decision_test
