#include "review_travel_collection_coordinator.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Travel collection coordinator contract failed: " << message << '\n';
    }
    return condition;
}

BackendLibraryFacetPage page(std::initializer_list<BackendLibraryFacet> items) {
    BackendLibraryFacetPage result;
    for (const BackendLibraryFacet& item : items) {
        result.items.push_back(item);
    }
    return result;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    bool saw_rules = false;
    ReviewTravelCollectionCoordinator coordinator({
        .page =
            [&saw_rules](
                const BackendLibraryPhotoFilter& filter,
                const BackendLibraryFacetKind kind,
                const BackendLibraryFacetCursor&,
                const std::uint32_t
            ) {
                if (kind == BackendLibraryFacetKind::City && filter.country_key.isEmpty()) {
                    return page({
                        {QStringLiteral("cn\u001fshanghai\u001fshanghai"),
                         QStringLiteral("Shanghai · China"),
                         2},
                        {QStringLiteral("jp\u001ftokyo\u001ftokyo"),
                         QStringLiteral("Tokyo · Japan"),
                         3},
                    });
                }
                saw_rules = filter.living_place_rules.size() == 2
                            && filter.living_place_rules[0].locality_key
                                   == QStringLiteral("cn\u001fshanghai\u001fshanghai")
                            && filter.living_place_rules[0].start_month == QStringLiteral("2020-01")
                            && filter.living_place_rules[1].locality_key
                                   == QStringLiteral("cn\u001fchengdu\u001fchengdu");
                if (kind == BackendLibraryFacetKind::Country) {
                    return page({{QStringLiteral("jp"), QStringLiteral("Japan"), 3}});
                }
                if (kind == BackendLibraryFacetKind::City
                    && filter.country_key == QStringLiteral("jp")) {
                    return page({
                        {QStringLiteral("jp\u001ftokyo\u001ftokyo"),
                         QStringLiteral("Tokyo · Japan"),
                         3},
                    });
                }
                return BackendLibraryFacetPage{};
            },
        .count =
            [&saw_rules](const BackendLibraryPhotoFilter& filter) {
                saw_rules = filter.living_place_rules.size() == 2;
                return std::uint64_t{3};
            },
    });

    QEventLoop loop;
    QObject::connect(
        &coordinator,
        &ReviewTravelCollectionCoordinator::projectionChanged,
        &loop,
        [&coordinator, &loop]() {
            if (!coordinator.busy()) {
                loop.quit();
            }
        }
    );
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    coordinator.refresh(
        {
            {
                .locality_key = QStringLiteral("cn\u001fshanghai\u001fshanghai"),
                .start_month = QStringLiteral("2020-01"),
            },
            {.locality_key = QStringLiteral("cn\u001fchengdu\u001fchengdu")},
        },
        7
    );
    loop.exec();

    const QVariantList groups = coordinator.groups();
    if (!require(!coordinator.busy(), "the projection reaches a terminal state")
        || !require(coordinator.errorText().isEmpty(), "the projection succeeds")
        || !require(saw_rules, "every Travel query carries all living-place periods")
        || !require(coordinator.photoCount() == 3, "Travel count is projected")
        || !require(
            coordinator.placeCandidates().size() == 2,
            "living-place candidates stay unfiltered"
        )
        || !require(groups.size() == 1, "one travel country is grouped")) {
        return EXIT_FAILURE;
    }
    const QVariantMap japan = groups.front().toMap();
    return require(
               japan.value(QStringLiteral("key")).toString() == QStringLiteral("jp"),
               "country identity is retained"
           )
                   && require(
                       japan.value(QStringLiteral("destinations")).toList().size() == 1,
                       "country contains its destination hierarchy"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
