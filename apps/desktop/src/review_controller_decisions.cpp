#include "review_controller.hpp"

// QML-facing photo decision and organization mutations.

void ReviewController::setPhotoFlag(const QString& photo_id, const QString& flag) {
    if (remote_library_coordinator_.ownsPresentationPhoto(photo_id)) {
        const auto desired_flag = review_decision_flag_from_name(flag);
        const auto current = model_.decisionFor(photo_id);
        if (desired_flag && current
            && remote_library_coordinator_.setDecision(
                photo_id,
                *desired_flag,
                current->rating
            )) {
            emit decisionChanged(
                photo_id,
                0,
                review_decision_flag_name(*desired_flag),
                current->rating
            );
        }
        return;
    }
    const bool admitted = !scanning() && !refreshing() && !query_coordinator_.pageRunning()
                          && !comparison_coordinator_.busy();
    static_cast<void>(decision_coordinator_.setFlag(photo_id, flag, admitted));
}

void ReviewController::setPhotoRating(const QString& photo_id, const int rating) {
    if (remote_library_coordinator_.ownsPresentationPhoto(photo_id)) {
        const auto current = model_.decisionFor(photo_id);
        const auto current_flag = current ? review_decision_flag_from_name(current->flag)
                                          : std::nullopt;
        if (current_flag
            && remote_library_coordinator_.setDecision(photo_id, *current_flag, rating)) {
            emit decisionChanged(photo_id, 0, current->flag, rating);
        }
        return;
    }
    const bool admitted = !scanning() && !refreshing() && !query_coordinator_.pageRunning()
                          && !comparison_coordinator_.busy();
    static_cast<void>(decision_coordinator_.setRating(photo_id, rating, admitted));
}

void ReviewController::setPhotoColorLabel(const QString& photo_id, const QString& color_label) {
    if (remote_library_coordinator_.ownsPresentationPhoto(photo_id)) {
        const auto current = model_.libraryStateFor(photo_id);
        if (current
            && remote_library_coordinator_.setAffinity(
                photo_id,
                current->liked,
                color_label
            )) {
            emit colorLabelChanged(photo_id, color_label);
        }
        return;
    }
    organization_coordinator_.setColorLabel(
        photo_id,
        color_label,
        !comparison_coordinator_.busy() && !decision_coordinator_.busy()
    );
}

void ReviewController::setPhotoLiked(const QString& photo_id, const bool liked) {
    if (remote_library_coordinator_.ownsPresentationPhoto(photo_id)) {
        const auto current = model_.libraryStateFor(photo_id);
        if (current
            && remote_library_coordinator_.setAffinity(
                photo_id,
                liked,
                current->color_label
            )) {
            emit likedChanged(photo_id, liked);
        }
        return;
    }
    organization_coordinator_.setLiked(
        photo_id,
        liked,
        !comparison_coordinator_.busy() && !decision_coordinator_.busy()
    );
}

void ReviewController::undoLastDecision() {
    const bool admitted = !scanning() && !refreshing() && !query_coordinator_.pageRunning()
                          && !comparison_coordinator_.busy();
    static_cast<void>(decision_coordinator_.undo(admitted));
}

void ReviewController::projectDecisionState(const BackendReviewDecisionState& state) {
    const QString flag = review_decision_flag_name(state.flag);
    const int rating = static_cast<int>(state.rating);
    const bool projected = model_.updateDecision(state.photo_id, state.head_sequence, flag, rating);
    (void)projected;
    emit decisionChanged(state.photo_id, state.head_sequence, flag, rating);
    if (filtered_model_.hasActiveServerFilter()) {
        scheduleFilterQuery();
    } else {
        refreshLibraryFacets();
    }
}
