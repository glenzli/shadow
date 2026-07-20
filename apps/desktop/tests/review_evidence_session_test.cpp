#include "review_evidence_session.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "review evidence session contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void one_in_flight_write_prevents_duplicate_actions() {
    ReviewEvidenceSession session;
    require(session.beginRecord(), "the first record must start");
    require(session.busy(), "an in-flight record must be observable");
    require(!session.beginRecord(), "a second record must be rejected while pending");
    require(!session.beginForget(), "forget must be rejected while a record is pending");
    session.fail();
    require(!session.busy(), "a failed write must release the pending state");
    require(session.activeCount() == 0, "a failed write must not invent evidence");
}

void successful_records_form_a_session_local_forget_stack() {
    ReviewEvidenceSession session;
    require(session.beginRecord(), "first record begins");
    require(session.completeRecord(QStringLiteral("event-a")), "first receipt completes");
    require(session.beginRecord(), "second record begins");
    require(session.completeRecord(QStringLiteral("event-b")), "second receipt completes");
    require(session.activeCount() == 2, "both durable receipts remain active");

    const auto target = session.beginForget();
    require(target && *target == QStringLiteral("event-b"), "undo targets the newest event");
    require(
        session.completeForget(QStringLiteral("event-b")),
        "matching forget receipt removes only the newest event"
    );
    require(session.activeCount() == 1 && session.canForget(), "older evidence remains undoable");
}

void malformed_receipts_fail_closed_without_losing_evidence() {
    ReviewEvidenceSession session;
    require(session.beginRecord(), "record begins");
    require(!session.completeRecord(QStringLiteral("   ")), "empty event id is rejected");
    require(session.activeCount() == 0 && !session.busy(), "bad record receipt is inert");

    require(session.beginRecord(), "valid record begins");
    require(session.completeRecord(QStringLiteral("event-a")), "valid record completes");
    const auto target = session.beginForget();
    require(target.has_value(), "forget begins");
    require(
        !session.completeForget(QStringLiteral("different-event")),
        "a mismatched forget receipt fails closed"
    );
    require(session.activeCount() == 1, "mismatched receipt must not drop evidence");
    require(session.canForget(), "failed forget remains retryable");
}

} // namespace

int main() {
    one_in_flight_write_prevents_duplicate_actions();
    successful_records_form_a_session_local_forget_stack();
    malformed_receipts_fail_closed_without_losing_evidence();
    return EXIT_SUCCESS;
}
