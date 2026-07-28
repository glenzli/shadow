#pragma once

#include "review_import_coordinator.hpp"

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

namespace review_import_test {

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Review import coordinator contract failed: " << message
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

struct ImportBackendState final {
    mutable std::mutex mutex;
    std::condition_variable condition;
    QVector<quint64> begun_ids;
    QVector<quint64> cancelled_ids;
    BackendScanProgress progress;
    BackendScanReport report;
    bool scan_entered = false;
    bool release_scan = false;
    bool cancel_result = true;
    bool fail_begin = false;
    bool fail_scan = false;
    bool fail_progress = false;
    bool fail_cancel = false;
};

inline ReviewImportCoordinator::Operations operations(
    const std::shared_ptr<ImportBackendState>& state
) {
    return {
        .begin =
            [state](const quint64 scan_id) {
                std::lock_guard lock(state->mutex);
                if (state->fail_begin) {
                    throw std::runtime_error("begin failed");
                }
                state->begun_ids.push_back(scan_id);
            },
        .scan =
            [state](const QString&, const quint64) {
                std::unique_lock lock(state->mutex);
                state->scan_entered = true;
                state->condition.notify_all();
                state->condition.wait(
                    lock,
                    [state]() { return state->release_scan; }
                );
                if (state->fail_scan) {
                    throw std::runtime_error("scan failed");
                }
                return state->report;
            },
        .progress =
            [state](const quint64) {
                std::lock_guard lock(state->mutex);
                if (state->fail_progress) {
                    throw std::runtime_error("progress failed");
                }
                return state->progress;
            },
        .cancel =
            [state](const quint64 scan_id) {
                std::lock_guard lock(state->mutex);
                if (state->fail_cancel) {
                    throw std::runtime_error("cancel failed");
                }
                state->cancelled_ids.push_back(scan_id);
                return state->cancel_result;
            },
    };
}

inline void wait_for_scan_entry(
    const std::shared_ptr<ImportBackendState>& state
) {
    std::unique_lock lock(state->mutex);
    state->condition.wait(lock, [state]() { return state->scan_entered; });
}

inline void release_scan(const std::shared_ptr<ImportBackendState>& state) {
    {
        std::lock_guard lock(state->mutex);
        state->release_scan = true;
    }
    state->condition.notify_all();
}

inline void set_progress(
    const std::shared_ptr<ImportBackendState>& state,
    BackendScanProgress progress
) {
    std::lock_guard lock(state->mutex);
    state->progress = std::move(progress);
}

inline int begun_count(const std::shared_ptr<ImportBackendState>& state) {
    std::lock_guard lock(state->mutex);
    return static_cast<int>(state->begun_ids.size());
}

} // namespace review_import_test
