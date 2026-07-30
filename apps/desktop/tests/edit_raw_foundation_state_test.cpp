#include "edit_raw_foundation_state.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using shadow::desktop::EditRawFoundationJobAcceptance;
using shadow::desktop::EditRawFoundationPhase;
using shadow::desktop::EditRawFoundationState;

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "RAW foundation editor state test failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool happy_path_contract() {
    EditRawFoundationState state;
    const auto generation = state.reset_context(true, false);
    if (!require(!generation.has_value(), "fresh context has no job to cancel")) {
        return false;
    }
    const auto context = state.begin_probe();
    if (!require(
            state.phase() == EditRawFoundationPhase::Checking && state.complete_probe(context, true)
                && state.can_start_job(),
            "available runtime admits one job"
        )
        || !require(state.begin_job(41, context), "exact context starts the job")
        || !require(
            state.accept_job_status(41, context, EditRawFoundationPhase::Running, 2'500, false)
                == EditRawFoundationJobAcceptance::Active,
            "running progress remains active"
        )
        || !require(
            state.accept_job_status(41, context, EditRawFoundationPhase::Ready, 10'000, false)
                == EditRawFoundationJobAcceptance::Terminal,
            "ready completion is terminal"
        )) {
        return false;
    }
    return require(
               state.materialized_ready() && !state.job_busy()
                   && state.completed_basis_points() == 10'000,
               "verified materialization survives job retirement"
           )
           && require(
               state.sync_recipe_enabled(true) && state.recipe_enabled(),
               "Recipe enablement is a separate non-destructive transition"
           )
           && require(
               state.sync_recipe_enabled(false) && state.materialized_ready(),
               "bypass preserves the rebuildable ready foundation"
           );
}

[[nodiscard]] bool stale_generation_contract() {
    EditRawFoundationState state;
    static_cast<void>(state.reset_context(true, false));
    const auto first_context = state.begin_probe();
    if (!require(state.complete_probe(first_context, true), "first probe completes")
        || !require(state.begin_job(71, first_context), "first job starts")) {
        return false;
    }

    const auto cancelled = state.reset_context(true, false);
    const auto second_context = state.begin_probe();
    return require(
               cancelled == 71 && second_context != first_context,
               "source switch returns the old token and advances generation"
           )
           && require(
               state.accept_job_status(
                   71,
                   first_context,
                   EditRawFoundationPhase::Ready,
                   10'000,
                   false
               ) == EditRawFoundationJobAcceptance::Stale,
               "late ready output cannot enable another photo"
           )
           && require(
               !state.complete_probe(first_context, true),
               "late runtime probe cannot replace the new context"
           );
}

[[nodiscard]] bool cancellation_and_failure_contract() {
    EditRawFoundationState state;
    static_cast<void>(state.reset_context(true, true));
    const auto context = state.begin_probe();
    if (!require(
            state.complete_probe(context, false)
                && state.phase() == EditRawFoundationPhase::Unavailable && state.recipe_enabled(),
            "saved Recipe intent survives unavailable runtime"
        )) {
        return false;
    }

    static_cast<void>(state.sync_recipe_enabled(false));
    static_cast<void>(state.begin_probe());
    if (!require(state.complete_probe(context, true), "retry probe succeeds")
        || !require(state.begin_job(83, context), "retry job starts")
        || !require(
            state.request_cancellation() == 83 && state.cancellation_requested(),
            "cancel returns the exact backend token"
        )) {
        return false;
    }
    return require(
               state.accept_job_status(83, context, EditRawFoundationPhase::Cancelled, 1'200, true)
                   == EditRawFoundationJobAcceptance::Terminal,
               "provider cancellation is terminal"
           )
           && require(
               state.phase() == EditRawFoundationPhase::Cancelled && !state.materialized_ready(),
               "cancelled inference never authors a ready foundation"
           );
}

} // namespace

int main() {
    return happy_path_contract() && stale_generation_contract()
                   && cancellation_and_failure_contract()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
