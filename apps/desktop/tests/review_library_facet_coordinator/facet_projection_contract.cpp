#include "library_facet_fixture.hpp"

namespace review_library_facet_test {

void run_facet_projection_contracts() {
    auto state = std::make_shared<FacetBackendState>();
    ReviewLibraryFacetCoordinator coordinator(operations(state));
    BackendLibraryPhotoFilter filter;
    filter.camera_key = QStringLiteral("nikon-z9");
    filter.album_id = QStringLiteral("favorites");

    coordinator.refresh(filter, 41);
    wait_until([&coordinator]() { return !coordinator.busy(); }, "five-facet projection completes");
    const auto months = coordinator.captureMonths();
    const auto cameras = coordinator.cameras();
    const auto lenses = coordinator.lenses();
    const auto countries = coordinator.countries();
    const auto cities = coordinator.cities();
    const auto system_counts = coordinator.systemCollectionCounts();
    require(
        months.size() == 1 && cameras.size() == 1 && lenses.size() == 1 && countries.size() == 1
            && cities.size() == 1,
        "one immutable query publishes all five bounded projections"
    );
    require(
        months.front().toMap().value(QStringLiteral("key")) == QStringLiteral("nikon-z9-month")
            && cameras.front().toMap().value(QStringLiteral("key"))
                   == QStringLiteral("nikon-z9-camera")
            && lenses.front().toMap().value(QStringLiteral("key"))
                   == QStringLiteral("nikon-z9-lens")
            && countries.front().toMap().value(QStringLiteral("key"))
                   == QStringLiteral("nikon-z9-country")
            && cities.front().toMap().value(QStringLiteral("key"))
                   == QStringLiteral("nikon-z9-city")
            && cameras.front().toMap().value(QStringLiteral("photoCount")).toULongLong() == 7,
        "projection preserves dimension identity, labels, and counts"
    );
    require(
        system_counts.value(QStringLiteral("available")).toBool()
            && system_counts.value(QStringLiteral("all")).toULongLong() == 41
            && system_counts.value(QStringLiteral("liked")).toULongLong() == 7
            && system_counts.value(QStringLiteral("fiveStar")).toULongLong() == 3
            && system_counts.value(QStringLiteral("recentImports")).toULongLong() == 11,
        "projection publishes global built-in collection counts"
    );
    {
        std::lock_guard lock(state->mutex);
        require(
            state->calls.size() == 5
                && state->calls.at(0).kind == BackendLibraryFacetKind::CaptureMonth
                && state->calls.at(1).kind == BackendLibraryFacetKind::Camera
                && state->calls.at(2).kind == BackendLibraryFacetKind::Lens
                && state->calls.at(3).kind == BackendLibraryFacetKind::Country
                && state->calls.at(4).kind == BackendLibraryFacetKind::City,
            "the batch queries each facet dimension exactly once"
        );
        for (const auto& call : state->calls) {
            require(
                call.camera_key == QStringLiteral("nikon-z9")
                    && call.album_id == QStringLiteral("favorites") && call.cursor_key.isEmpty()
                    && call.limit == 24,
                "each dimension receives the same filter and first-page bound"
            );
        }
        require(
            state->count_calls.size() == 4 && !state->count_calls.at(0).has_liked
                && !state->count_calls.at(0).has_minimum_rating
                && state->count_calls.at(0).album_id.isEmpty() && state->count_calls.at(1).has_liked
                && state->count_calls.at(1).liked && state->count_calls.at(1).album_id.isEmpty()
                && state->count_calls.at(2).has_minimum_rating
                && state->count_calls.at(2).minimum_rating == 5
                && state->count_calls.at(2).album_id.isEmpty()
                && state->count_calls.at(3).recent_imports
                && state->count_calls.at(3).album_id.isEmpty(),
            "built-in collection counts stay global and use exact filters"
        );
    }
}

} // namespace review_library_facet_test
