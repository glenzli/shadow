#pragma once

#include "edit_display_scope_task.hpp"

#include <QFuture>
#include <QThreadPool>

struct EditWorkAdmissionState final {
    bool active = false;
    bool state_task_running = false;
    bool preview_running = false;
    bool preview_scheduled = false;
    bool detail_running = false;
    bool detail_scheduled = false;
    bool warmup_running = false;
    bool scope_running = false;
    bool scope_pending = false;
};

// The controller owns generation checks and cancellation. This owner admits
// auxiliary work only between foreground jobs and limits scope analysis to one
// low-priority worker outside Qt's shared foreground thread pool.
class EditWorkScheduler final {
  public:
    EditWorkScheduler();

    [[nodiscard]] static bool admitsDisplayScope(const EditWorkAdmissionState& state) noexcept;
    [[nodiscard]] static bool admitsIdleDetailWarmup(const EditWorkAdmissionState& state) noexcept;
    [[nodiscard]] QFuture<EditDisplayScopeTaskResult>
    runDisplayScope(EditDisplayScopeTaskInput input);

  private:
    QThreadPool analysis_pool_;
};
