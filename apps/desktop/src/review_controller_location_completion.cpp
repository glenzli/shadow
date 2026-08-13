#include "review_controller.hpp"

#include <cstdint>

QVariantList ReviewController::locationCompletionGroups() const {
    return location_completion_coordinator_.groups();
}

bool ReviewController::locationCompletionBusy() const noexcept {
    return location_completion_coordinator_.busy();
}

bool ReviewController::locationCompletionTruncated() const noexcept {
    return location_completion_coordinator_.truncated();
}

QString ReviewController::locationCompletionErrorText() const {
    return location_completion_coordinator_.errorText();
}

void ReviewController::requestLocationCompletion(
    const qlonglong capture_start_unix_seconds,
    const qlonglong capture_end_unix_seconds
) {
    const auto start = static_cast<std::int64_t>(capture_start_unix_seconds);
    const auto end = static_cast<std::int64_t>(capture_end_unix_seconds);
    if (start < 0 || end < 0 || (start > 0 && end > 0 && start > end)) {
        return;
    }
    location_completion_coordinator_.request(currentLibraryFilter(), start, end);
}
