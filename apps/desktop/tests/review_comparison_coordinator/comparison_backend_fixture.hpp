#pragma once

#include "review_comparison_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace review_comparison_test {

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr
            << "review comparison coordinator contract failed: "
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

class FakeComparisonBackend final {
public:
    [[nodiscard]] ReviewComparisonCoordinator::Operations operations() {
        return {
            .prepare =
                [this](
                    const QString& left_visual_handle,
                    const QString& right_visual_handle
                ) {
                    ++prepare_calls;
                    prepared_left = left_visual_handle;
                    prepared_right = right_visual_handle;
                    if (throw_prepare) {
                        throw std::runtime_error("sentinel prepare failure");
                    }
                    return BackendReviewComparisonPresentation{
                        .presentation_id = QStringLiteral("presentation-a"),
                        .left_request_ticket = QStringLiteral("ticket-left"),
                        .right_request_ticket = QStringLiteral("ticket-right"),
                    };
                },
            .confirm_ready =
                [this](
                    const QString& presentation_id,
                    const QString& left_request_ticket,
                    const QString& right_request_ticket
                ) {
                    ++confirm_calls;
                    confirmed_presentation = presentation_id;
                    confirmed_left = left_request_ticket;
                    confirmed_right = right_request_ticket;
                    if (throw_confirm) {
                        throw std::runtime_error("sentinel confirm failure");
                    }
                },
            .cancel =
                [this](const QString& presentation_id) {
                    ++cancel_calls;
                    cancelled_presentation = presentation_id;
                    if (throw_cancel) {
                        throw std::runtime_error("sentinel cancel failure");
                    }
                },
            .record =
                [this](
                    const QString& presentation_id,
                    const BackendPairwiseOutcome outcome
                ) {
                    ++record_calls;
                    if (record_started != nullptr) {
                        record_started->release();
                    }
                    if (release_record != nullptr) {
                        release_record->acquire();
                    }
                    if (throw_record) {
                        throw std::runtime_error("sentinel record failure");
                    }
                    std::scoped_lock lock(record_mutex);
                    recorded_presentations.push_back(presentation_id);
                    recorded_outcomes.push_back(outcome);
                    const QString event_id = record_event_override.value_or(
                        QStringLiteral("event-%1").arg(record_calls.load())
                    );
                    return BackendFeedbackReceipt{
                        .event_id = event_id,
                        .sequence =
                            static_cast<std::uint64_t>(record_calls.load() + 10),
                        .occurred_at_ms = 100,
                    };
                },
            .forget =
                [this](const QString& event_id) {
                    ++forget_calls;
                    if (forget_started != nullptr) {
                        forget_started->release();
                    }
                    if (release_forget != nullptr) {
                        release_forget->acquire();
                    }
                    if (throw_forget) {
                        throw std::runtime_error("sentinel forget failure");
                    }
                    forgotten_event_id = event_id;
                    return BackendForgetReceipt{
                        .fact_id = QStringLiteral("forget-fact"),
                        .target_event_id =
                            forget_target_override.value_or(event_id),
                        .sequence = 99,
                        .occurred_at_ms = 200,
                    };
                },
        };
    }

    int prepare_calls = 0;
    int confirm_calls = 0;
    int cancel_calls = 0;
    std::atomic<int> record_calls = 0;
    std::atomic<int> forget_calls = 0;
    QString prepared_left;
    QString prepared_right;
    QString confirmed_presentation;
    QString confirmed_left;
    QString confirmed_right;
    QString cancelled_presentation;
    QString forgotten_event_id;
    std::mutex record_mutex;
    std::vector<QString> recorded_presentations;
    std::vector<BackendPairwiseOutcome> recorded_outcomes;
    std::optional<QString> record_event_override;
    std::optional<QString> forget_target_override;
    QSemaphore* record_started = nullptr;
    QSemaphore* release_record = nullptr;
    QSemaphore* forget_started = nullptr;
    QSemaphore* release_forget = nullptr;
    bool throw_prepare = false;
    bool throw_confirm = false;
    bool throw_cancel = false;
    bool throw_record = false;
    bool throw_forget = false;
};

[[nodiscard]] inline ReviewComparisonCoordinator make_coordinator(
    FakeComparisonBackend& backend
) {
    return ReviewComparisonCoordinator(
        backend.operations(),
        [](const QString& ticket) {
            return QStringLiteral("image://review/%1").arg(ticket);
        }
    );
}

} // namespace review_comparison_test
