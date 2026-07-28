#pragma once

class QApplication;
class ReviewController;

namespace DesktopSmoke {

void awaitFirstComparisonMutation(
    QApplication& application,
    ReviewController& controller,
    bool forget_recorded_comparison
);
void awaitFirstDecisionMutation(
    QApplication& application,
    ReviewController& controller,
    bool undo_first_decision
);

} // namespace DesktopSmoke
