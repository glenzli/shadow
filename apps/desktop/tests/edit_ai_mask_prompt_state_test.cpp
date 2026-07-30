#include "edit_ai_mask_prompt_state.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using shadow::desktop::AiMaskPromptCompletion;
using shadow::desktop::AiMaskPromptMutationResult;
using shadow::desktop::AiMaskPromptPoint;
using shadow::desktop::AiMaskPromptPolarity;
using shadow::desktop::EditAiMaskPromptState;

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "AI-mask prompt state test failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] AiMaskPromptPoint foreground(const double x, const double y) {
    return {
        .x = x,
        .y = y,
        .polarity = AiMaskPromptPolarity::Foreground,
    };
}

[[nodiscard]] bool pointContract() {
    EditAiMaskPromptState state;
    if (!require(
            state.append_point(foreground(-0.1, 0.5)) == AiMaskPromptMutationResult::InvalidPoint,
            "out-of-bounds points are rejected"
        )
        || !require(
            state.append_point(foreground(std::numeric_limits<double>::quiet_NaN(), 0.5))
                == AiMaskPromptMutationResult::InvalidPoint,
            "non-finite points are rejected"
        )) {
        return false;
    }

    for (std::size_t index = 0; index < state.maximum_points; ++index) {
        const double coordinate =
            static_cast<double>(index) / static_cast<double>(state.maximum_points);
        if (!require(
                state.append_point(foreground(coordinate, 0.5))
                    == AiMaskPromptMutationResult::Applied,
                "bounded points are accepted"
            )) {
            return false;
        }
    }
    return require(
               state.append_point(foreground(0.5, 0.5))
                   == AiMaskPromptMutationResult::PointLimitReached,
               "the SAM prompt limit is enforced before dispatch"
           )
           && require(
               state.undo_point() == AiMaskPromptMutationResult::Applied
                   && state.points().size() == state.maximum_points - 1,
               "undo removes exactly one accepted point"
           )
           && require(
               state.clear_points() == AiMaskPromptMutationResult::Applied
                   && state.points().empty(),
               "clear retires the current point set"
           );
}

[[nodiscard]] bool generationContract() {
    EditAiMaskPromptState state;
    (void)state.append_point(foreground(0.3, 0.4));
    const auto first = state.begin_request(41);
    if (!require(
            first.has_value() && first->points.size() == 1 && state.busy(),
            "dispatch receives one immutable prompt snapshot"
        )
        || !require(
            state.append_point(foreground(0.7, 0.8)) == AiMaskPromptMutationResult::Busy,
            "point mutation is frozen while inference is active"
        )) {
        return false;
    }
    return require(
               state.complete_request(first->job_token, first->generation)
                   == AiMaskPromptCompletion::Current,
               "an exact active result is current"
           )
           && require(!state.busy(), "current completion retires the active job")
           && require(
               state.begin_request(42).has_value(),
               "the same prompt can generate another staged candidate before apply"
           )
           && require(
               state.complete_request(42, first->generation) == AiMaskPromptCompletion::Current,
               "a repeated candidate remains tied to the unchanged prompt generation"
           )
           && require(
               state.complete_request(first->job_token, first->generation)
                   == AiMaskPromptCompletion::Stale,
               "one provider result cannot be applied twice"
           );
}

[[nodiscard]] bool staleContract() {
    EditAiMaskPromptState state;
    (void)state.append_point(foreground(0.2, 0.3));
    const auto cancelled = state.begin_request(71);
    if (!require(cancelled.has_value(), "cancellable request starts")) {
        return false;
    }
    const auto cancelled_token = state.cancel_active_request();
    if (!require(
            cancelled_token == cancelled->job_token && !state.busy(),
            "cancellation returns the exact backend job token"
        )
        || !require(
            state.complete_request(cancelled->job_token, cancelled->generation)
                == AiMaskPromptCompletion::Stale,
            "a late cancelled result is stale"
        )) {
        return false;
    }

    const auto restarted = state.begin_request(72);
    if (!require(
            restarted.has_value() && restarted->generation != cancelled->generation,
            "restarting the same points receives a new generation"
        )) {
        return false;
    }
    const auto context_job = state.reset_context();
    return require(
               context_job == restarted->job_token && state.points().empty() && !state.busy(),
               "photo context reset returns the job to cancel and clears points"
           )
           && require(
               state.complete_request(restarted->job_token, restarted->generation)
                   == AiMaskPromptCompletion::Stale,
               "a result from the previous photo context is stale"
           );
}

} // namespace

int main() {
    return pointContract() && generationContract() && staleContract() ? EXIT_SUCCESS : EXIT_FAILURE;
}
