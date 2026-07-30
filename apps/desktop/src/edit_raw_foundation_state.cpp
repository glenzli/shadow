#include "edit_raw_foundation_state.hpp"

#include <algorithm>
#include <limits>

namespace shadow::desktop {
namespace {

[[nodiscard]] constexpr bool is_terminal(const EditRawFoundationPhase phase) noexcept {
    return phase == EditRawFoundationPhase::Ready || phase == EditRawFoundationPhase::Unavailable
           || phase == EditRawFoundationPhase::Cancelled || phase == EditRawFoundationPhase::Failed;
}

[[nodiscard]] constexpr bool is_job_phase(const EditRawFoundationPhase phase) noexcept {
    return phase == EditRawFoundationPhase::Queued || phase == EditRawFoundationPhase::Planning
           || phase == EditRawFoundationPhase::Running || is_terminal(phase);
}

} // namespace

std::optional<std::uint64_t>
EditRawFoundationState::reset_context(const bool active, const bool recipe_enabled) noexcept {
    const auto cancelled_job = active_job_token_;
    context_generation_ = context_generation_ == std::numeric_limits<std::uint64_t>::max()
                              ? 1
                              : context_generation_ + 1;
    active_job_token_.reset();
    phase_ = active ? EditRawFoundationPhase::Checking : EditRawFoundationPhase::Off;
    completed_basis_points_ = 0;
    active_ = active;
    runtime_known_ = false;
    runtime_available_ = false;
    recipe_enabled_ = recipe_enabled;
    materialized_ready_ = false;
    cancellation_requested_ = false;
    return cancelled_job;
}

std::uint64_t EditRawFoundationState::begin_probe() noexcept {
    if (active_ && !active_job_token_) {
        runtime_known_ = false;
        runtime_available_ = false;
        phase_ = EditRawFoundationPhase::Checking;
        completed_basis_points_ = 0;
    }
    return context_generation_;
}

bool EditRawFoundationState::complete_probe(
    const std::uint64_t context_generation,
    const bool available
) noexcept {
    if (!active_ || context_generation != context_generation_ || active_job_token_) {
        return false;
    }
    runtime_known_ = true;
    runtime_available_ = available;
    phase_ = available ? EditRawFoundationPhase::Available : EditRawFoundationPhase::Unavailable;
    completed_basis_points_ = 0;
    return true;
}

bool EditRawFoundationState::begin_job(
    const std::uint64_t job_token,
    const std::uint64_t context_generation
) noexcept {
    if (job_token == 0 || context_generation != context_generation_ || !can_start_job()) {
        return false;
    }
    active_job_token_ = job_token;
    phase_ = EditRawFoundationPhase::Queued;
    completed_basis_points_ = 0;
    cancellation_requested_ = false;
    return true;
}

EditRawFoundationJobAcceptance EditRawFoundationState::accept_job_status(
    const std::uint64_t job_token,
    const std::uint64_t context_generation,
    const EditRawFoundationPhase phase,
    const std::uint16_t completed_basis_points,
    const bool cancellation_requested
) noexcept {
    if (!active_ || context_generation != context_generation_ || !active_job_token_
        || *active_job_token_ != job_token || !is_job_phase(phase)) {
        return EditRawFoundationJobAcceptance::Stale;
    }
    phase_ = phase;
    completed_basis_points_ = std::max(completed_basis_points_, completed_basis_points);
    cancellation_requested_ = cancellation_requested_ || cancellation_requested;
    if (!is_terminal(phase_)) {
        return EditRawFoundationJobAcceptance::Active;
    }
    active_job_token_.reset();
    if (phase_ == EditRawFoundationPhase::Ready) {
        materialized_ready_ = true;
    }
    return EditRawFoundationJobAcceptance::Terminal;
}

std::optional<std::uint64_t> EditRawFoundationState::request_cancellation() noexcept {
    if (!active_job_token_) {
        return std::nullopt;
    }
    cancellation_requested_ = true;
    return active_job_token_;
}

bool EditRawFoundationState::fail_active_job(
    const std::uint64_t job_token,
    const std::uint64_t context_generation
) noexcept {
    if (!active_ || context_generation != context_generation_ || !active_job_token_
        || *active_job_token_ != job_token) {
        return false;
    }
    active_job_token_.reset();
    phase_ = cancellation_requested_ ? EditRawFoundationPhase::Cancelled
                                     : EditRawFoundationPhase::Failed;
    return true;
}

bool EditRawFoundationState::sync_recipe_enabled(const bool enabled) noexcept {
    if (recipe_enabled_ == enabled) {
        return false;
    }
    recipe_enabled_ = enabled;
    return true;
}

bool EditRawFoundationState::active() const noexcept {
    return active_;
}

bool EditRawFoundationState::runtime_known() const noexcept {
    return runtime_known_;
}

bool EditRawFoundationState::runtime_available() const noexcept {
    return runtime_known_ && runtime_available_;
}

bool EditRawFoundationState::recipe_enabled() const noexcept {
    return recipe_enabled_;
}

bool EditRawFoundationState::materialized_ready() const noexcept {
    return materialized_ready_;
}

bool EditRawFoundationState::job_busy() const noexcept {
    return active_job_token_.has_value();
}

bool EditRawFoundationState::cancellation_requested() const noexcept {
    return cancellation_requested_;
}

bool EditRawFoundationState::can_start_job() const noexcept {
    return active_ && runtime_available() && !recipe_enabled_ && !active_job_token_;
}

std::uint64_t EditRawFoundationState::context_generation() const noexcept {
    return context_generation_;
}

std::optional<std::uint64_t> EditRawFoundationState::active_job_token() const noexcept {
    return active_job_token_;
}

EditRawFoundationPhase EditRawFoundationState::phase() const noexcept {
    return phase_;
}

std::uint16_t EditRawFoundationState::completed_basis_points() const noexcept {
    return completed_basis_points_;
}

} // namespace shadow::desktop
