#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace shadow::desktop {

enum class AiMaskPromptPolarity : std::uint8_t {
    Foreground,
    Background,
};

struct AiMaskPromptPoint final {
    double x = 0.0;
    double y = 0.0;
    AiMaskPromptPolarity polarity = AiMaskPromptPolarity::Foreground;

    [[nodiscard]] bool operator==(const AiMaskPromptPoint&) const = default;
};

struct AiMaskPromptSnapshot final {
    std::uint64_t job_token = 0;
    std::uint64_t generation = 0;
    std::vector<AiMaskPromptPoint> points;
};

enum class AiMaskPromptMutationResult : std::uint8_t {
    Applied,
    Busy,
    Empty,
    PointLimitReached,
    InvalidPoint,
    GenerationExhausted,
};

enum class AiMaskPromptCompletion : std::uint8_t {
    Current,
    Stale,
};

// UI-thread state for one provider-neutral point-prompt session. Provider jobs
// receive immutable snapshots; prompt mutation and cancellation advance the
// generation so a late result cannot be applied to a newer visual context.
class EditAiMaskPromptState final {
  public:
    static constexpr std::size_t maximum_points = 16;

    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] const std::vector<AiMaskPromptPoint>& points() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> active_job_token() const noexcept;

    [[nodiscard]] AiMaskPromptMutationResult append_point(AiMaskPromptPoint point);
    [[nodiscard]] AiMaskPromptMutationResult undo_point() noexcept;
    [[nodiscard]] AiMaskPromptMutationResult clear_points() noexcept;

    [[nodiscard]] AiMaskPromptMutationResult invalidate_selection() noexcept;

    [[nodiscard]] std::optional<AiMaskPromptSnapshot>
    begin_request(std::uint64_t job_token, bool allow_empty = false);

    // Cancelling returns the provider job token that the controller must pass
    // to the backend cancellation boundary.
    [[nodiscard]] std::optional<std::uint64_t> cancel_active_request() noexcept;

    // Photo/layer/representation changes are stronger than prompt clearing:
    // they always advance the generation, even if there are no prompt points.
    [[nodiscard]] std::optional<std::uint64_t> reset_context() noexcept;

    // Current means the result matches the exact active job and generation.
    // Either outcome retires a matching active job exactly once.
    [[nodiscard]] AiMaskPromptCompletion
    complete_request(std::uint64_t job_token, std::uint64_t generation) noexcept;

  private:
    struct ActiveRequest final {
        std::uint64_t job_token = 0;
        std::uint64_t generation = 0;
    };

    [[nodiscard]] bool advance_generation() noexcept;

    std::uint64_t generation_ = 1;
    std::vector<AiMaskPromptPoint> points_;
    std::optional<ActiveRequest> active_request_;
};

} // namespace shadow::desktop
