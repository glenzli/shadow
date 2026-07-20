#pragma once

#include <cstdint>

enum class EditPreviewKind : std::uint8_t {
    Current,
    NeutralBefore,
};

struct EditPreviewGeneration final {
    EditPreviewKind kind = EditPreviewKind::Current;
    std::uint64_t photo = 0;
    std::uint64_t current_revision = 0;
};

struct NeutralBeforeStartState final {
    bool requested = false;
    bool active = false;
    bool state_task_running = false;
    bool current_rendering = false;
    bool before_rendering = false;
    bool current_scheduled = false;
    std::uint64_t settled_current_revision = 0;
    std::uint64_t current_revision = 0;
};

[[nodiscard]] constexpr bool accepts_edit_preview(
    const EditPreviewGeneration result,
    const std::uint64_t current_photo,
    const std::uint64_t current_revision
) noexcept {
    if (result.photo != current_photo) {
        return false;
    }
    return result.kind == EditPreviewKind::NeutralBefore
        || result.current_revision == current_revision;
}

[[nodiscard]] constexpr bool can_start_neutral_before(
    const NeutralBeforeStartState state
) noexcept {
    return state.requested && state.active && !state.state_task_running
        && !state.current_rendering && !state.before_rendering
        && !state.current_scheduled
        && state.settled_current_revision == state.current_revision;
}
