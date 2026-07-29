#pragma once

#include "edit_mask_coverage_contract.hpp"

#include <compare>
#include <cstdint>
#include <optional>

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

enum class EditPreviewTerminal : std::uint8_t {
    Completed,
    Cancelled,
    Failed,
};

enum class EditPreviewPresentationApi : std::uint8_t {
    Unavailable = 0U,
    Software = 1U,
    Metal = 2U,
    Other = 3U,
};

// One immutable scene-graph capability observation. It is runtime-only
// presentation state: it follows a preview generation into the store/factory,
// but never participates in Recipe or renderer cache identity.
struct EditPreviewPresentationBinding final {
    std::uint64_t epoch = 0U;
    std::uintptr_t window_identity = 0U;
    EditPreviewPresentationApi api = EditPreviewPresentationApi::Unavailable;
    std::uintptr_t device_handle = 0U;
    bool initialized = false;

    auto operator<=>(const EditPreviewPresentationBinding&) const = default;
};

[[nodiscard]] constexpr bool edit_preview_terminal_admits_publication(
    const EditPreviewTerminal terminal
) noexcept {
    return terminal == EditPreviewTerminal::Completed;
}

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
    /// Session-issued cancellation/publication terminal claim.
    std::uint64_t render_token = 0;
    /// Recipe mutation identity, intentionally independent from a preview
    /// generation advanced only to request another selected-node coverage.
    std::uint64_t recipe_revision = 0;
    std::optional<EditMaskCoverageRequest> mask_coverage_request;
    EditPreviewPresentationBinding presentation_binding;

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

struct EditPreviewCancellationState final {
    bool force = false;
    bool current_rendering = false;
    EditPreviewPolicy in_flight_policy = EditPreviewPolicy::Settled;
    bool gesture_active = false;
    bool first_interactive_frame_presented = false;
};

/// Repeated samples protect the first interactive frame in a gesture. Every
/// other stale overview is cancellable, including a forced gesture end,
/// photo/window transition, settled frame, or Neutral Before frame.
[[nodiscard]] constexpr bool should_cancel_edit_preview(
    const EditPreviewCancellationState state
) noexcept {
    if (state.force) {
        return true;
    }
    const bool protected_first_interactive =
        state.current_rendering
        && state.in_flight_policy == EditPreviewPolicy::Interactive
        && state.gesture_active
        && !state.first_interactive_frame_presented;
    return !protected_first_interactive;
}

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
