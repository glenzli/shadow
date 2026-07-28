#include "shared_grade_fixture.hpp"

namespace review_shared_grade_test {

void run_failure_admission_contracts() {
    auto state = std::make_shared<SharedGradeBackendState>();
    ReviewSharedGradeCoordinator coordinator(operations(state));
    int refresh_requests = 0;
    QObject::connect(
        &coordinator,
        &ReviewSharedGradeCoordinator::libraryRefreshRequested,
        [&refresh_requests]() { ++refresh_requests; }
    );

    const QVariantMap missing_layer = coordinator.apply(
        QString{},
        {
            target(QStringLiteral("photo-1"), QStringLiteral("/raw/one.nef")),
        }
    );
    const QVariantMap invalid_targets = coordinator.apply(
        QStringLiteral("layer-1"),
        {
            target(QStringLiteral("photo-1"), QString{}),
        }
    );
    require(
        state->apply_calls.isEmpty()
            && missing_layer.value(QStringLiteral("requested")).toUInt() == 0
            && invalid_targets.value(QStringLiteral("requested")).toUInt() == 0,
        "missing layer identity or complete targets is rejected locally"
    );

    state->fail_nodes = true;
    coordinator.refresh();
    require(
        coordinator.statusMessage().translated()
            == QStringLiteral(
                "Could not load shared Grade Nodes · shared nodes failed"
            ),
        "snapshot failures publish exact localized diagnostics"
    );

    state->fail_apply = true;
    const QVariantMap failed = coordinator.apply(
        QStringLiteral("layer-1"),
        {
            target(QStringLiteral("photo-1"), QStringLiteral("/raw/one.nef")),
            target(QStringLiteral("photo-2"), QStringLiteral("/raw/two.nef")),
        }
    );
    require(
        failed.value(QStringLiteral("requested")).toInt() == 2
            && failed.value(QStringLiteral("updated")).toInt() == 0
            && failed.value(QStringLiteral("unchanged")).toInt() == 0
            && failed.value(QStringLiteral("failed")).toInt() == 2
            && failed.value(QStringLiteral("errors")).toStringList()
                == QStringList{QStringLiteral("shared apply failed")}
            && refresh_requests == 0
            && coordinator.statusMessage().translated()
                == QStringLiteral(
                    "Could not apply shared Grade Node · shared apply failed"
                ),
        "apply failures retain the normalized request count without invalidating rows"
    );

    bool rejected_incomplete_operations = false;
    try {
        ReviewSharedGradeCoordinator invalid({
            .nodes = []() { return QVector<BackendSharedGradeNode>{}; },
        });
    } catch (const std::invalid_argument&) {
        rejected_incomplete_operations = true;
    }
    require(
        rejected_incomplete_operations,
        "construction rejects an incomplete backend boundary"
    );
}

} // namespace review_shared_grade_test
