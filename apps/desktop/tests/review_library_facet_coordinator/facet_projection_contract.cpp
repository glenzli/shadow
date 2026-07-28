#include "library_facet_fixture.hpp"

namespace review_library_facet_test {

void run_facet_projection_contracts() {
    auto state = std::make_shared<FacetBackendState>();
    ReviewLibraryFacetCoordinator coordinator(operations(state));
    BackendLibraryPhotoFilter filter;
    filter.camera_key = QStringLiteral("nikon-z9");
    filter.album_id = QStringLiteral("favorites");

    coordinator.refresh(filter, 41);
    wait_until(
        [&coordinator]() { return !coordinator.busy(); },
        "three-facet projection completes"
    );
    const auto months = coordinator.captureMonths();
    const auto cameras = coordinator.cameras();
    const auto lenses = coordinator.lenses();
    require(
        months.size() == 1 && cameras.size() == 1 && lenses.size() == 1,
        "one immutable query publishes all three bounded projections"
    );
    require(
        months.front().toMap().value(QStringLiteral("key"))
                == QStringLiteral("nikon-z9-month")
            && cameras.front().toMap().value(QStringLiteral("key"))
                == QStringLiteral("nikon-z9-camera")
            && lenses.front().toMap().value(QStringLiteral("key"))
                == QStringLiteral("nikon-z9-lens")
            && cameras.front().toMap().value(QStringLiteral("photoCount"))
                .toULongLong() == 7,
        "projection preserves dimension identity, labels, and counts"
    );
    {
        std::lock_guard lock(state->mutex);
        require(
            state->calls.size() == 3
                && state->calls.at(0).kind
                    == BackendLibraryFacetKind::CaptureMonth
                && state->calls.at(1).kind
                    == BackendLibraryFacetKind::Camera
                && state->calls.at(2).kind
                    == BackendLibraryFacetKind::Lens,
            "the batch queries each facet dimension exactly once"
        );
        for (const auto& call : state->calls) {
            require(
                call.camera_key == QStringLiteral("nikon-z9")
                    && call.album_id == QStringLiteral("favorites")
                    && call.cursor_key.isEmpty() && call.limit == 24,
                "each dimension receives the same filter and first-page bound"
            );
        }
    }
}

} // namespace review_library_facet_test
