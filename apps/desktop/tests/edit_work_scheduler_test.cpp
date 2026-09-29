#include "edit_work_scheduler.hpp"

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    EditWorkAdmissionState idle{.active = true};
    if (!require(EditWorkScheduler::admitsDisplayScope(idle), "idle editor admits scopes")
        || !require(
            EditWorkScheduler::admitsIdleDetailWarmup(idle),
            "idle editor admits detail warmup"
        )) {
        return EXIT_FAILURE;
    }

    auto preview = idle;
    preview.preview_scheduled = true;
    auto detail = idle;
    detail.detail_running = true;
    auto warmup = idle;
    warmup.warmup_running = true;
    auto active_scope = idle;
    active_scope.scope_running = true;
    if (!require(
            !EditWorkScheduler::admitsDisplayScope(preview),
            "queued foreground preview runs before scope"
        )
        || !require(
            !EditWorkScheduler::admitsDisplayScope(detail),
            "foreground detail runs before scope"
        )
        || !require(
            !EditWorkScheduler::admitsDisplayScope(warmup),
            "scope does not join active source warmup"
        )
        || !require(
            !EditWorkScheduler::admitsDisplayScope(active_scope),
            "scope analysis has one active job"
        )) {
        return EXIT_FAILURE;
    }

    auto pending_scope = idle;
    pending_scope.scope_pending = true;
    auto state_write = idle;
    state_write.state_task_running = true;
    if (!require(
            EditWorkScheduler::admitsDisplayScope(pending_scope),
            "latest pending scope can start when foreground is idle"
        )
        || !require(
            !EditWorkScheduler::admitsIdleDetailWarmup(pending_scope),
            "scope analysis precedes idle full-detail warmup"
        )
        || !require(
            EditWorkScheduler::admitsDisplayScope(state_write),
            "autosave does not strand a pending scope"
        )
        || !require(
            !EditWorkScheduler::admitsIdleDetailWarmup(state_write),
            "state writes exclude idle full-detail warmup"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
