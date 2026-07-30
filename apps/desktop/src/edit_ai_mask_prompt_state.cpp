#include "edit_ai_mask_prompt_state.hpp"

#include <cmath>
#include <limits>

namespace shadow::desktop {

std::uint64_t EditAiMaskPromptState::generation() const noexcept {
    return generation_;
}

const std::vector<AiMaskPromptPoint>& EditAiMaskPromptState::points() const noexcept {
    return points_;
}

bool EditAiMaskPromptState::busy() const noexcept {
    return active_request_.has_value();
}

std::optional<std::uint64_t>
EditAiMaskPromptState::active_job_token() const noexcept {
    if (!active_request_) {
        return std::nullopt;
    }
    return active_request_->job_token;
}

AiMaskPromptMutationResult EditAiMaskPromptState::append_point(
    const AiMaskPromptPoint point
) {
    if (busy()) {
        return AiMaskPromptMutationResult::Busy;
    }
    if (!std::isfinite(point.x) || !std::isfinite(point.y)
        || point.x < 0.0 || point.x > 1.0
        || point.y < 0.0 || point.y > 1.0) {
        return AiMaskPromptMutationResult::InvalidPoint;
    }
    if (points_.size() >= maximum_points) {
        return AiMaskPromptMutationResult::PointLimitReached;
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return AiMaskPromptMutationResult::GenerationExhausted;
    }
    points_.push_back(point);
    ++generation_;
    return AiMaskPromptMutationResult::Applied;
}

AiMaskPromptMutationResult EditAiMaskPromptState::undo_point() noexcept {
    if (busy()) {
        return AiMaskPromptMutationResult::Busy;
    }
    if (points_.empty()) {
        return AiMaskPromptMutationResult::Empty;
    }
    if (!advance_generation()) {
        return AiMaskPromptMutationResult::GenerationExhausted;
    }
    points_.pop_back();
    return AiMaskPromptMutationResult::Applied;
}

AiMaskPromptMutationResult EditAiMaskPromptState::clear_points() noexcept {
    if (busy()) {
        return AiMaskPromptMutationResult::Busy;
    }
    if (points_.empty()) {
        return AiMaskPromptMutationResult::Empty;
    }
    if (!advance_generation()) {
        return AiMaskPromptMutationResult::GenerationExhausted;
    }
    points_.clear();
    return AiMaskPromptMutationResult::Applied;
}

std::optional<AiMaskPromptSnapshot> EditAiMaskPromptState::begin_request(
    const std::uint64_t job_token
) {
    if (job_token == 0 || busy() || points_.empty()) {
        return std::nullopt;
    }
    AiMaskPromptSnapshot snapshot{
        .job_token = job_token,
        .generation = generation_,
        .points = points_,
    };
    active_request_ = ActiveRequest{
        .job_token = job_token,
        .generation = generation_,
    };
    return snapshot;
}

std::optional<std::uint64_t>
EditAiMaskPromptState::cancel_active_request() noexcept {
    if (!active_request_ || !advance_generation()) {
        return std::nullopt;
    }
    const std::uint64_t job_token = active_request_->job_token;
    active_request_.reset();
    return job_token;
}

std::optional<std::uint64_t> EditAiMaskPromptState::reset_context() noexcept {
    const std::optional<std::uint64_t> job_token = active_job_token();
    if (!advance_generation()) {
        return std::nullopt;
    }
    active_request_.reset();
    points_.clear();
    return job_token;
}

AiMaskPromptCompletion EditAiMaskPromptState::complete_request(
    const std::uint64_t job_token,
    const std::uint64_t generation
) noexcept {
    if (!active_request_
        || active_request_->job_token != job_token
        || active_request_->generation != generation) {
        return AiMaskPromptCompletion::Stale;
    }
    active_request_.reset();
    return generation == generation_
        ? AiMaskPromptCompletion::Current
        : AiMaskPromptCompletion::Stale;
}

bool EditAiMaskPromptState::advance_generation() noexcept {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    ++generation_;
    return true;
}

} // namespace shadow::desktop
