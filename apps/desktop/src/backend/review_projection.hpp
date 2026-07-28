#pragma once

#include "../desktop_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <stdexcept>

namespace desktop_backend_projection {

[[nodiscard]] inline shadow::desktop::FfiPairwiseOutcome ffi_outcome(
    const BackendPairwiseOutcome outcome
) {
    switch (outcome) {
    case BackendPairwiseOutcome::LeftPreferred:
        return shadow::desktop::FfiPairwiseOutcome::LeftPreferred;
    case BackendPairwiseOutcome::RightPreferred:
        return shadow::desktop::FfiPairwiseOutcome::RightPreferred;
    case BackendPairwiseOutcome::KeepBoth:
        return shadow::desktop::FfiPairwiseOutcome::KeepBoth;
    case BackendPairwiseOutcome::KeepNeither:
        return shadow::desktop::FfiPairwiseOutcome::KeepNeither;
    case BackendPairwiseOutcome::CannotCompare:
        return shadow::desktop::FfiPairwiseOutcome::CannotCompare;
    }
    throw std::invalid_argument("unknown pairwise outcome");
}

[[nodiscard]] inline BackendReviewDecisionFlag decision_flag(
    const shadow::desktop::FfiDecisionFlag flag
) {
    switch (flag) {
    case shadow::desktop::FfiDecisionFlag::Unflagged:
        return BackendReviewDecisionFlag::Unflagged;
    case shadow::desktop::FfiDecisionFlag::Picked:
        return BackendReviewDecisionFlag::Picked;
    case shadow::desktop::FfiDecisionFlag::Rejected:
        return BackendReviewDecisionFlag::Rejected;
    }
    throw std::invalid_argument("unknown Review decision flag");
}

[[nodiscard]] inline shadow::desktop::FfiDecisionFlag ffi_decision_flag(
    const BackendReviewDecisionFlag flag
) {
    switch (flag) {
    case BackendReviewDecisionFlag::Unflagged:
        return shadow::desktop::FfiDecisionFlag::Unflagged;
    case BackendReviewDecisionFlag::Picked:
        return shadow::desktop::FfiDecisionFlag::Picked;
    case BackendReviewDecisionFlag::Rejected:
        return shadow::desktop::FfiDecisionFlag::Rejected;
    }
    throw std::invalid_argument("unknown Review decision flag");
}

} // namespace desktop_backend_projection
