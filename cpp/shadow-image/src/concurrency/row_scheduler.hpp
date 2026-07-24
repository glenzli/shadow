#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace shadow::image::detail {

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
