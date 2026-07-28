#include "review_controller.hpp"

// QML-facing admission and routing for one comparison transaction.

QVariantMap ReviewController::prepareComparison(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) {
    if (comparison_coordinator_.busy() || decision_coordinator_.busy()
        || scanning() || refreshing() || query_coordinator_.pageRunning()) {
        return {};
    }
    return comparison_coordinator_.prepare(
        left_visual_handle,
        right_visual_handle
    );
}

bool ReviewController::confirmComparisonReady(
    const QString& presentation_id,
    const QString& left_request_ticket,
    const QString& right_request_ticket
) {
    if (decision_coordinator_.busy()) {
        return false;
    }
    return comparison_coordinator_.confirmReady(
        presentation_id,
        left_request_ticket,
        right_request_ticket
    );
}

void ReviewController::cancelComparison(const QString& presentation_id) {
    if (decision_coordinator_.busy()) {
        return;
    }
    comparison_coordinator_.cancel(presentation_id);
}

void ReviewController::recordComparison(
    const QString& presentation_id,
    const int outcome
) {
    const bool admitted = !decision_coordinator_.busy() && !scanning()
        && !refreshing() && !query_coordinator_.pageRunning();
    static_cast<void>(comparison_coordinator_.record(
        presentation_id,
        outcome,
        admitted
    ));
}

void ReviewController::undoLastComparison() {
    if (decision_coordinator_.busy() || scanning() || refreshing()
        || query_coordinator_.pageRunning()) {
        return;
    }
    static_cast<void>(comparison_coordinator_.forgetLast());
}
