#include "edit_work_scheduler.hpp"

#include <QtConcurrentRun>

#include <utility>

EditWorkScheduler::EditWorkScheduler() {
    analysis_pool_.setMaxThreadCount(1);
    analysis_pool_.setThreadPriority(QThread::LowPriority);
}

bool EditWorkScheduler::admitsDisplayScope(const EditWorkAdmissionState& state) noexcept {
    return state.active && !state.preview_running && !state.preview_scheduled
           && !state.detail_running && !state.detail_scheduled && !state.warmup_running
           && !state.scope_running;
}

bool EditWorkScheduler::admitsIdleDetailWarmup(const EditWorkAdmissionState& state) noexcept {
    return admitsDisplayScope(state) && !state.state_task_running && !state.scope_pending;
}

QFuture<EditDisplayScopeTaskResult>
EditWorkScheduler::runDisplayScope(EditDisplayScopeTaskInput input) {
    return QtConcurrent::run(&analysis_pool_, run_edit_display_scope_task, std::move(input));
}
