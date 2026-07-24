#pragma once

#include <compare>
#include <cstdint>

enum class EditPreviewKind : std::uint8_t {
    Current,
    NeutralBefore,
};

// Rendering intent is explicit instead of inferred from proxy dimensions.
// That keeps transient slider frames out of durable caches and lets the UI
// treat missing analysis as an intentional low-latency contract.
enum class EditPreviewPolicy : std::uint8_t {
    Interactive,
    Settled,
    NeutralBefore,
};

[[nodiscard]] constexpr EditPreviewKind edit_preview_kind(
    const EditPreviewPolicy policy
) noexcept {
    return policy == EditPreviewPolicy::NeutralBefore
        ? EditPreviewKind::NeutralBefore : EditPreviewKind::Current;
}

[[nodiscard]] constexpr bool edit_preview_requires_analysis(
    const EditPreviewPolicy policy
) noexcept {
    return policy != EditPreviewPolicy::Interactive;
}

[[nodiscard]] constexpr bool edit_preview_admits_durable_cache(
    const EditPreviewPolicy policy
) noexcept {
    return policy == EditPreviewPolicy::Settled;
}

[[nodiscard]] constexpr bool edit_preview_requires_display_diagnostics(
    const EditPreviewPolicy policy
) noexcept {
    return policy != EditPreviewPolicy::Interactive;
}

struct EditPreviewGeneration final {
    EditPreviewPolicy policy = EditPreviewPolicy::Settled;
    std::uint64_t photo = 0;
    std::uint64_t current_revision = 0;

    [[nodiscard]] constexpr EditPreviewKind kind() const noexcept {
        return edit_preview_kind(policy);
    }
};

// Full-resolution detail has one more source of staleness than the bounded
// overview: panning or zooming changes the exact tile set without changing
// either the photo or its Recipe. Results are therefore accepted only when
// all three generations still match.
struct EditDetailGeneration final {
    std::uint64_t photo = 0;
    std::uint64_t recipe_revision = 0;
    std::uint64_t viewport_revision = 0;

    auto operator<=>(const EditDetailGeneration&) const = default;
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
    return result.kind() == EditPreviewKind::NeutralBefore
        || result.current_revision == current_revision;
}

// A completed current-preview frame can still improve visual feedback while a
// newer Recipe revision is queued. It is safe to present only for the same
// photo and never as a neutral-before frame. Exact acceptance above remains the
// gate for declaring the preview settled and publishing its histogram.
[[nodiscard]] constexpr bool can_present_edit_preview(
    const EditPreviewGeneration result,
    const std::uint64_t current_photo,
    const std::uint64_t current_revision
) noexcept {
    return result.kind() == EditPreviewKind::Current
        && result.photo == current_photo
        && result.current_revision <= current_revision;
}

[[nodiscard]] constexpr bool accepts_edit_detail(
    const EditDetailGeneration result,
    const std::uint64_t current_photo,
    const std::uint64_t current_recipe_revision,
    const std::uint64_t current_viewport_revision
) noexcept {
    return result.photo == current_photo
        && result.recipe_revision == current_recipe_revision
        && result.viewport_revision == current_viewport_revision;
}

[[nodiscard]] constexpr bool can_start_neutral_before(
    const NeutralBeforeStartState state
) noexcept {
    return state.requested && state.active && !state.state_task_running
        && !state.current_rendering && !state.before_rendering
        && !state.current_scheduled
        && state.settled_current_revision == state.current_revision;
}
