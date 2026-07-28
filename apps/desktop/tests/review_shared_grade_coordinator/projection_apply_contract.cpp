#include "shared_grade_fixture.hpp"

namespace review_shared_grade_test {

void run_projection_apply_contracts() {
    auto state = std::make_shared<SharedGradeBackendState>();
    state->nodes = {
        node(
            QStringLiteral("layer-1"),
            QStringLiteral("revision-1"),
            7,
            QStringLiteral("Portrait")
        ),
    };
    state->receipt = {
        .requested = 2,
        .updated = 1,
        .unchanged = 1,
    };
    ReviewSharedGradeCoordinator coordinator(operations(state));
    int node_changes = 0;
    int refresh_requests = 0;
    QObject::connect(
        &coordinator,
        &ReviewSharedGradeCoordinator::nodesChanged,
        [&node_changes]() { ++node_changes; }
    );
    QObject::connect(
        &coordinator,
        &ReviewSharedGradeCoordinator::libraryRefreshRequested,
        [&refresh_requests]() { ++refresh_requests; }
    );

    coordinator.refresh();
    const QVariantList projected = coordinator.nodes();
    require(
        node_changes == 1 && projected.size() == 1
            && projected.front().toMap().value(QStringLiteral("layerId"))
                == QStringLiteral("layer-1")
            && projected.front().toMap().value(QStringLiteral("revisionId"))
                == QStringLiteral("revision-1")
            && projected.front()
                    .toMap()
                    .value(QStringLiteral("revisionNumber"))
                    .toUInt()
                == 7
            && projected.front().toMap().value(QStringLiteral("label"))
                == QStringLiteral("Portrait"),
        "refresh projects the complete public shared-node summary"
    );
    coordinator.refresh();
    require(
        node_changes == 1 && state->node_calls == 2,
        "an unchanged authoritative snapshot does not emit a false change"
    );

    const QVariantMap receipt = coordinator.apply(
        QStringLiteral("layer-1"),
        {
            target(QStringLiteral("photo-1"), QStringLiteral("/raw/one.nef")),
            target(QStringLiteral("photo-1"), QStringLiteral("/raw/duplicate.nef")),
            target(QStringLiteral("photo-2"), QStringLiteral("/raw/two.nef")),
            target(QString{}, QStringLiteral("/raw/missing-id.nef")),
            target(QStringLiteral("missing-path"), QString{}),
            QStringLiteral("not-a-map"),
        }
    );
    require(
        state->apply_calls.size() == 1
            && state->apply_calls.front().layer_id
                == QStringLiteral("layer-1")
            && state->apply_calls.front().targets.size() == 2
            && state->apply_calls.front().targets.front().photo_id
                == QStringLiteral("photo-1")
            && state->apply_calls.front().targets.front().source_path
                == QStringLiteral("/raw/one.nef")
            && state->apply_calls.front().targets.back().photo_id
                == QStringLiteral("photo-2"),
        "apply admits unique complete photo/source targets in stable order"
    );
    require(
        receipt.value(QStringLiteral("requested")).toUInt() == 2
            && receipt.value(QStringLiteral("updated")).toUInt() == 1
            && receipt.value(QStringLiteral("unchanged")).toUInt() == 1
            && receipt.value(QStringLiteral("failed")).toUInt() == 0
            && receipt.value(QStringLiteral("errors")).toStringList().isEmpty()
            && refresh_requests == 1
            && coordinator.statusMessage().translated()
                == QStringLiteral(
                    "Shared Grade Node linked to 1 photos · 1 already current"
                ),
        "a successful receipt is projected completely and invalidates visible rows"
    );

    state->receipt = {
        .requested = 2,
        .updated = 1,
        .failed = 1,
        .errors = {QStringLiteral("photo-2 rejected")},
    };
    const QVariantMap partial = coordinator.apply(
        QStringLiteral("layer-1"),
        {
            target(QStringLiteral("photo-1"), QStringLiteral("/raw/one.nef")),
            target(QStringLiteral("photo-2"), QStringLiteral("/raw/two.nef")),
        }
    );
    require(
        partial.value(QStringLiteral("failed")).toUInt() == 1
            && partial.value(QStringLiteral("errors")).toStringList()
                == QStringList{QStringLiteral("photo-2 rejected")}
            && refresh_requests == 2
            && coordinator.statusMessage().translated()
                == QStringLiteral(
                    "Shared Grade Node linked to 1 photos · 1 failed"
                ),
        "a partial backend receipt preserves its errors and refreshes changed rows"
    );
}

} // namespace review_shared_grade_test
