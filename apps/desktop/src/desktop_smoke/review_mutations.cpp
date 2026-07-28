#include "review_mutations.hpp"

#include "../review_controller.hpp"

#include <QApplication>
#include <QDebug>
#include <QTimer>

#include <memory>

namespace DesktopSmoke {

void awaitFirstComparisonMutation(
    QApplication& application,
    ReviewController& controller,
    const bool forget_recorded_comparison
) {
    auto comparison_succeeded = std::make_shared<bool>(false);
    QObject::connect(
        &controller,
        &ReviewController::comparisonRecorded,
        &application,
        [&application, &controller, forget_recorded_comparison,
         comparison_succeeded]() {
            if (forget_recorded_comparison) {
                controller.undoLastComparison();
            } else {
                *comparison_succeeded = true;
                QTimer::singleShot(50, &application, &QCoreApplication::quit);
            }
        }
    );
    QObject::connect(
        &controller,
        &ReviewController::comparisonForgotten,
        &application,
        [&application, comparison_succeeded]() {
            *comparison_succeeded = true;
            QTimer::singleShot(50, &application, &QCoreApplication::quit);
        }
    );
    QTimer::singleShot(
        30'000,
        &application,
        [&application, &controller, comparison_succeeded]() {
            if (!*comparison_succeeded) {
                qCritical()
                    << "Comparison smoke timed out"
                    << "rows" << controller.reviewModel()->rowCount()
                    << "scanning" << controller.scanning()
                    << "refreshing" << controller.refreshing()
                    << "busy" << controller.comparisonBusy()
                    << "session evidence"
                    << controller.sessionEvidenceCount()
                    << "status" << controller.comparisonStatusText();
            }
            application.exit(*comparison_succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
        }
    );
}

void awaitFirstDecisionMutation(
    QApplication& application,
    ReviewController& controller,
    const bool undo_first_decision
) {
    auto decision_succeeded = std::make_shared<bool>(false);
    QObject::connect(
        &controller,
        &ReviewController::decisionChanged,
        &application,
        [&application, &controller, undo_first_decision, decision_succeeded](
            const QString&,
            const qulonglong,
            const QString& flag,
            const int
        ) {
            if (flag != QStringLiteral("picked")
                || !controller.canUndoDecision()) {
                return;
            }
            if (undo_first_decision) {
                QTimer::singleShot(
                    0,
                    &controller,
                    &ReviewController::undoLastDecision
                );
            } else {
                *decision_succeeded = true;
                QTimer::singleShot(50, &application, &QCoreApplication::quit);
            }
        }
    );
    QObject::connect(
        &controller,
        &ReviewController::decisionUndone,
        &application,
        [&application, decision_succeeded]() {
            *decision_succeeded = true;
            QTimer::singleShot(50, &application, &QCoreApplication::quit);
        }
    );
    QTimer::singleShot(
        30'000,
        &application,
        [&application, &controller, decision_succeeded]() {
            if (!*decision_succeeded) {
                qCritical()
                    << "Decision smoke failed"
                    << "rows" << controller.reviewModel()->rowCount()
                    << "scanning" << controller.scanning()
                    << "refreshing" << controller.refreshing()
                    << "busy" << controller.decisionBusy()
                    << "can undo" << controller.canUndoDecision()
                    << "status" << controller.decisionStatusText();
            }
            application.exit(*decision_succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
        }
    );
}

} // namespace DesktopSmoke
