#include "comparison_backend_fixture.hpp"

namespace review_comparison_test {
namespace {

void presentation_verification_and_cancellation_form_one_owner() {
    FakeComparisonBackend backend;
    auto coordinator = make_coordinator(backend);

    require(
        coordinator.prepare({}, QStringLiteral("right")).isEmpty(),
        "missing handles must not prepare a presentation"
    );
    require(
        coordinator.statusText()
            == QStringLiteral("Choose two verified visuals before comparing"),
        "missing handles must publish the existing terminal status"
    );

    const QVariantMap presentation = coordinator.prepare(
        QStringLiteral("handle-left"),
        QStringLiteral("handle-right")
    );
    require(backend.prepare_calls == 1, "prepare must call the backend once");
    require(
        backend.prepared_left == QStringLiteral("handle-left")
            && backend.prepared_right == QStringLiteral("handle-right"),
        "prepare must preserve both exact visual handles"
    );
    require(
        presentation.value(QStringLiteral("presentationId")).toString()
            == QStringLiteral("presentation-a"),
        "presentation identity must survive projection"
    );
    require(
        presentation.value(QStringLiteral("leftSource")).toString()
            == QStringLiteral("image://review/ticket-left")
            && presentation.value(QStringLiteral("rightSource")).toString()
                == QStringLiteral("image://review/ticket-right"),
        "both comparison tickets must use the generation-aware resolver"
    );
    require(
        coordinator.confirmReady(
            QStringLiteral("presentation-a"),
            QStringLiteral("ticket-left"),
            QStringLiteral("ticket-right")
        ),
        "matching frame receipts must confirm"
    );
    require(
        backend.confirmed_presentation == QStringLiteral("presentation-a")
            && backend.confirmed_left == QStringLiteral("ticket-left")
            && backend.confirmed_right == QStringLiteral("ticket-right"),
        "frame verification must preserve all three exact identities"
    );
    coordinator.cancel(QStringLiteral("presentation-a"));
    require(
        backend.cancel_calls == 1
            && backend.cancelled_presentation
                == QStringLiteral("presentation-a"),
        "cancel must close the exact presentation"
    );
    require(
        coordinator.statusText()
            == QStringLiteral("Comparison presentation closed"),
        "cancel must publish its terminal status"
    );
}

void synchronous_failures_are_terminal_and_diagnostic() {
    FakeComparisonBackend backend;
    auto coordinator = make_coordinator(backend);

    backend.throw_prepare = true;
    require(
        coordinator.prepare(
            QStringLiteral("left"),
            QStringLiteral("right")
        ).isEmpty(),
        "prepare failure must not expose a partial presentation"
    );
    require(
        coordinator.statusText().contains(
            QStringLiteral("sentinel prepare failure")
        ),
        "prepare failure must retain the backend diagnostic"
    );

    backend.throw_confirm = true;
    require(
        !coordinator.confirmReady(
            QStringLiteral("presentation"),
            QStringLiteral("left-ticket"),
            QStringLiteral("right-ticket")
        ),
        "frame verification failure must be explicit"
    );
    require(
        coordinator.statusText().contains(
            QStringLiteral("sentinel confirm failure")
        ),
        "frame verification failure must retain the backend diagnostic"
    );

    backend.throw_cancel = true;
    coordinator.cancel(QStringLiteral("presentation"));
    require(
        coordinator.statusText().contains(
            QStringLiteral("sentinel cancel failure")
        ),
        "cancel failure must retain the backend diagnostic"
    );
}

} // namespace

void run_presentation_contracts() {
    presentation_verification_and_cancellation_form_one_owner();
    synchronous_failures_are_terminal_and_diagnostic();
}

} // namespace review_comparison_test
