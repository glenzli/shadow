#include "library_organization_fixture.hpp"

namespace review_library_organization_test {

void run_mutation_projection_contracts() {
    auto state = std::make_shared<OrganizationBackendState>();
    state->current.insert(
        QStringLiteral("photo"),
        {
            .liked = false,
            .color_label = QStringLiteral("red"),
        }
    );
    ReviewLibraryOrganizationCoordinator coordinator(operations(state));
    int projections = 0;
    QObject::connect(
        &coordinator,
        &ReviewLibraryOrganizationCoordinator::stateProjected,
        [&projections](
            const QString&,
            const bool,
            const QString&
        ) { ++projections; }
    );

    coordinator.setColorLabel(
        QStringLiteral("photo"),
        QStringLiteral(" BLUE "),
        true
    );
    wait_until(
        [&coordinator]() { return !coordinator.busy(); },
        "color-label mutation completes"
    );
    {
        std::lock_guard lock(state->mutex);
        require(
            state->mutations.size() == 1
                && !state->mutations.front().liked
                && state->mutations.front().color_label
                    == QStringLiteral("blue"),
            "color mutation normalizes the label and preserves Like"
        );
        require(
            state->projections.size() == 1
                && state->projections.front().updated_at_ms == 77,
            "the authoritative receipt is projected exactly once"
        );
    }
    require(
        projections == 1
            && coordinator.statusMessage().translated()
                == QStringLiteral("Library organization updated"),
        "successful projection emits the public state and localized status"
    );

    coordinator.setLiked(QStringLiteral("photo"), true, true);
    wait_until(
        [&coordinator]() { return !coordinator.busy(); },
        "Like mutation completes"
    );
    {
        std::lock_guard lock(state->mutex);
        require(
            state->mutations.size() == 2
                && state->mutations.back().liked
                && state->mutations.back().color_label
                    == QStringLiteral("blue"),
            "Like mutation preserves the projected color label"
        );
    }

    coordinator.setLiked(QStringLiteral("photo"), true, true);
    coordinator.setColorLabel(
        QStringLiteral("photo"),
        QStringLiteral("blue"),
        true
    );
    coordinator.setLiked(QStringLiteral("photo"), false, false);
    require(
        mutation_count(state) == 2,
        "no-op and externally blocked requests never start workers"
    );

    coordinator.setLiked(QStringLiteral("missing"), true, true);
    require(
        coordinator.statusMessage().translated()
            == QStringLiteral(
                "Select a loaded photo before changing its Like state"
            ),
        "missing visible state produces the precise local admission message"
    );
}

} // namespace review_library_organization_test
