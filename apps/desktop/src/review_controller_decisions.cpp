#include "review_controller.hpp"

// QML-facing photo decision and organization mutations.

void ReviewController::setPhotoFlag(
    const QString& photo_id,
    const QString& flag
) {
    const bool admitted = !scanning() && !refreshing()
        && !query_coordinator_.pageRunning()
        && !comparison_coordinator_.busy();
    static_cast<void>(
        decision_coordinator_.setFlag(photo_id, flag, admitted)
    );
}

void ReviewController::setPhotoRating(
    const QString& photo_id,
    const int rating
) {
    const bool admitted = !scanning() && !refreshing()
        && !query_coordinator_.pageRunning()
        && !comparison_coordinator_.busy();
    static_cast<void>(
        decision_coordinator_.setRating(photo_id, rating, admitted)
    );
}

void ReviewController::setPhotoColorLabel(
    const QString& photo_id,
    const QString& color_label
) {
    organization_coordinator_.setColorLabel(
        photo_id,
        color_label,
        !comparison_coordinator_.busy() && !decision_coordinator_.busy()
    );
}

void ReviewController::setPhotoLiked(
    const QString& photo_id,
    const bool liked
) {
    organization_coordinator_.setLiked(
        photo_id,
        liked,
        !comparison_coordinator_.busy() && !decision_coordinator_.busy()
    );
}

void ReviewController::undoLastDecision() {
    const bool admitted = !scanning() && !refreshing()
        && !query_coordinator_.pageRunning()
        && !comparison_coordinator_.busy();
    static_cast<void>(decision_coordinator_.undo(admitted));
}

void ReviewController::projectDecisionState(
    const BackendReviewDecisionState& state
) {
    const QString flag = review_decision_flag_name(state.flag);
    const int rating = static_cast<int>(state.rating);
    const bool projected = model_.updateDecision(
        state.photo_id,
        state.head_sequence,
        flag,
        rating
    );
    (void)projected;
    emit decisionChanged(state.photo_id, state.head_sequence, flag, rating);
    if (filtered_model_.hasActiveServerFilter()) {
        scheduleFilterQuery();
    }
}
