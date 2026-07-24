#include "row_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

thread_local bool is_image_worker = false;

[[nodiscard]] std::size_t default_worker_count() noexcept {
    const std::size_t hardware = std::max(1U, std::thread::hardware_concurrency());
    // Leave capacity for the UI/event loop and source I/O. A fixed upper bound avoids a very
    // wide workstation turning each small preview operation into dozens of competing tasks.
    return std::clamp<std::size_t>(hardware > 2U ? hardware - 2U : 1U, 1U, 12U);
}

class ImageWorkPool final {
public:
    ImageWorkPool() {
        const std::size_t count = default_worker_count();
        workers_.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            workers_.emplace_back([this](const std::stop_token stop) {
                is_image_worker = true;
                while (!stop.stop_requested()) {
                    std::function<void()> work;
                    {
                        std::unique_lock lock(mutex_);
                        ready_.wait(lock, stop, [this] { return !queue_.empty(); });
                        if (stop.stop_requested()) {
                            break;
                        }
                        work = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    work();
                }
                is_image_worker = false;
            });
        }
    }

    ImageWorkPool(const ImageWorkPool&) = delete;
    ImageWorkPool& operator=(const ImageWorkPool&) = delete;

    ~ImageWorkPool() {
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        ready_.notify_all();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return workers_.size();
    }

    void enqueue(std::function<void()> work) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(work));
        }
        ready_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::jthread> workers_;
};

[[nodiscard]] ImageWorkPool& image_work_pool() {
    static ImageWorkPool pool;
    return pool;
}

struct RowJob final {
    std::uint32_t row_count = 0U;
    std::uint32_t chunk_size = 1U;
    const RowRangeTask* task = nullptr;
    std::atomic<std::uint32_t> next_row{0U};
    std::atomic<std::size_t> participants{0U};
    std::mutex completion_mutex;
    std::condition_variable completion;
    std::mutex exception_mutex;
    std::exception_ptr exception;
};

void run_row_job(const std::shared_ptr<RowJob>& job) noexcept {
    try {
        // Every participant owns a copy of the callable. Pixel transforms are normally
        // stateless, but preserving per-participant callable state keeps the shared executor
        // no less safe than the former per-operation std::thread implementation.
        const RowRangeTask task = *job->task;
        while (true) {
            const std::uint32_t first = job->next_row.fetch_add(
                job->chunk_size,
                std::memory_order_relaxed
            );
            if (first >= job->row_count) {
                break;
            }
            const std::uint32_t last = std::min(job->row_count, first + job->chunk_size);
            task(first, last);
        }
    } catch (...) {
        std::lock_guard lock(job->exception_mutex);
        if (job->exception == nullptr) {
            job->exception = std::current_exception();
        }
        // Stop handing out work after the first failure. Already-running chunks complete before
        // the caller observes the exception, keeping every captured source buffer alive.
        job->next_row.store(job->row_count, std::memory_order_relaxed);
    }
    if (job->participants.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        std::lock_guard lock(job->completion_mutex);
        job->completion.notify_one();
    }
}

} // namespace

void parallel_for_rows(
    const std::uint32_t row_count,
    const std::uint32_t minimum_rows_per_chunk,
    const RowRangeTask& task
) {
    if (row_count == 0U) {
        return;
    }
    const std::uint32_t chunk_size = std::max(1U, minimum_rows_per_chunk);
    auto& pool = image_work_pool();
    const std::size_t available_chunks =
        (static_cast<std::size_t>(row_count) + chunk_size - 1U) / chunk_size;
    const std::size_t participant_count = std::min(pool.size() + 1U, available_chunks);
    if (participant_count <= 1U || is_image_worker) {
        task(0U, row_count);
        return;
    }

    auto job = std::make_shared<RowJob>();
    job->row_count = row_count;
    job->chunk_size = chunk_size;
    job->task = &task;
    job->participants.store(participant_count, std::memory_order_relaxed);
    for (std::size_t index = 1U; index < participant_count; ++index) {
        pool.enqueue([job] { run_row_job(job); });
    }
    run_row_job(job);

    {
        std::unique_lock lock(job->completion_mutex);
        job->completion.wait(lock, [&job] {
            return job->participants.load(std::memory_order_acquire) == 0U;
        });
    }
    if (job->exception != nullptr) {
        std::rethrow_exception(job->exception);
    }
}

std::size_t image_worker_count() noexcept {
    return image_work_pool().size();
}

} // namespace shadow::image::detail
