#include "review_location_reference_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Location-reference coordinator contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate> void wait_until(Predicate predicate, const char* const message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

BackendLocationReferenceLibrary library(
    const QString& id,
    const QString& path,
    const std::int64_t offset,
    const std::uint64_t anchors
) {
    return {
        .id = id,
        .root_path = path,
        .clock_offset_seconds = offset,
        .indexed_at_unix_ms = 123,
        .anchor_count = anchors,
    };
}

void run_contract() {
    bool rejected_incomplete_operations = false;
    try {
        ReviewLocationReferenceCoordinator coordinator({});
    } catch (const std::invalid_argument&) {
        rejected_incomplete_operations = true;
    }
    require(rejected_incomplete_operations, "construction requires every reference operation");

    QVector<BackendLocationReferenceLibrary> state;
    int add_count = 0;
    int remove_count = 0;
    ReviewLocationReferenceCoordinator coordinator({
        .libraries = [&state]() { return state; },
        .add_or_rescan = [&state, &add_count](const QString& path, const std::int64_t offset) {
            ++add_count;
            state = {library(QStringLiteral("phone"), path, offset, 3)};
            return state.front();
        },
        .remove = [&state, &remove_count](const QString& id) {
            ++remove_count;
            const bool found = !state.isEmpty() && state.front().id == id;
            if (found) {
                state.clear();
            }
            return found;
        },
    });

    coordinator.refresh();
    wait_until([&coordinator]() { return !coordinator.busy(); }, "refresh reaches a terminal state");
    require(coordinator.libraries().isEmpty(), "the initial reference list is empty");

    int anchors_changed = 0;
    QObject::connect(
        &coordinator,
        &ReviewLocationReferenceCoordinator::anchorsChanged,
        [&anchors_changed]() { ++anchors_changed; }
    );
    coordinator.addOrRescan(QStringLiteral("/photos/phone"), 90);
    wait_until([&coordinator]() { return !coordinator.busy(); }, "add reaches a terminal state");
    require(add_count == 1 && anchors_changed == 1, "indexing publishes an anchor change");
    const QVariantMap added = coordinator.libraries().front().toMap();
    require(
        added.value(QStringLiteral("rootPath")).toString() == QStringLiteral("/photos/phone")
            && added.value(QStringLiteral("clockOffsetSeconds")).toLongLong() == 90
            && added.value(QStringLiteral("anchorCount")).toULongLong() == 3
            && added.value(QStringLiteral("rootUrl")).toString().startsWith(QStringLiteral("file:")),
        "the projected library retains its evidence-only details"
    );

    coordinator.remove(QStringLiteral("phone"));
    wait_until([&coordinator]() { return !coordinator.busy(); }, "remove reaches a terminal state");
    require(
        remove_count == 1 && anchors_changed == 2 && coordinator.libraries().isEmpty(),
        "removal republishes the anchor set without changing the main Library"
    );
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    run_contract();
    return EXIT_SUCCESS;
}
