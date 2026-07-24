#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <stop_token>

namespace shadow::image::detail {

class RowExecutionCancelled final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "row-oriented image operation was cancelled";
    }
};

// Installs a stop token for row-oriented stages entered synchronously by this thread. The
// scheduler copies it into each submitted job, so fixed-pool workers do not depend on inherited
// thread-local state and unrelated renders remain independent.
class ScopedRowCancellation final {
public:
    explicit ScopedRowCancellation(std::stop_token token) noexcept;
    ScopedRowCancellation(const ScopedRowCancellation&) = delete;
    ScopedRowCancellation& operator=(const ScopedRowCancellation&) = delete;
    ~ScopedRowCancellation();

private:
    std::stop_token previous_;
};

[[nodiscard]] bool row_cancellation_requested() noexcept;
void throw_if_row_cancelled();

// Process-wide bounded CPU executor for row-oriented image work. It replaces per-node
// thread creation and also prevents nested image operations from oversubscribing the machine:
// a scheduler worker executes a nested request inline while ordinary callers share the same
// fixed worker set.
using RowRangeTask = std::function<void(std::uint32_t first_row, std::uint32_t last_row)>;

void parallel_for_rows(
    std::uint32_t row_count,
    std::uint32_t minimum_rows_per_chunk,
    const RowRangeTask& task
);

[[nodiscard]] std::size_t image_worker_count() noexcept;

} // namespace shadow::image::detail
