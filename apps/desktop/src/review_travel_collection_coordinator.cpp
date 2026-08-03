#include "review_travel_collection_coordinator.hpp"

#include <QtConcurrentRun>

#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t HOME_CANDIDATE_LIMIT = 96;
constexpr std::uint32_t TRAVEL_COUNTRY_LIMIT = 48;
constexpr std::uint32_t TRAVEL_DESTINATION_LIMIT = 48;

} // namespace

ReviewTravelCollectionCoordinator::ReviewTravelCollectionCoordinator(
    Operations operations,
    QObject* const parent
) : QObject(parent), operations_(std::move(operations)) {
    if (!operations_.page || !operations_.count) {
        throw std::invalid_argument("Travel collection facet and count operations are required");
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewTravelCollectionCoordinator::finishTask
    );
}

ReviewTravelCollectionCoordinator::~ReviewTravelCollectionCoordinator() {
    watcher_.waitForFinished();
}

QVariantList ReviewTravelCollectionCoordinator::homeCandidates() const {
    return facetVariants(home_candidates_);
}

QVariantList ReviewTravelCollectionCoordinator::groups() const {
    QVariantList values;
    values.reserve(groups_.size());
    for (const CountryGroup& group : groups_) {
        values.push_back(
            QVariantMap{
                {QStringLiteral("key"), group.country.key},
                {QStringLiteral("label"), group.country.label},
                {
                    QStringLiteral("photoCount"),
                    QVariant::fromValue(static_cast<qulonglong>(group.country.photo_count)),
                },
                {QStringLiteral("destinations"), facetVariants(group.destinations)},
            }
        );
    }
    return values;
}

qulonglong ReviewTravelCollectionCoordinator::photoCount() const noexcept {
    return static_cast<qulonglong>(photo_count_);
}

bool ReviewTravelCollectionCoordinator::busy() const noexcept {
    return task_running_;
}

QString ReviewTravelCollectionCoordinator::errorText() const {
    return error_text_;
}

void ReviewTravelCollectionCoordinator::refresh(
    QString home_locality_key,
    const quint64 library_generation
) {
    requested_home_locality_key_ = home_locality_key.trimmed().toLower();
    requested_library_generation_ = library_generation;
    if (task_running_) {
        refresh_pending_ = true;
        return;
    }
    startTask();
}

ReviewTravelCollectionCoordinator::TaskResult ReviewTravelCollectionCoordinator::runTask(
    Operations operations,
    QString home_locality_key,
    const quint64 library_generation,
    const quint64 request_id
) {
    TaskResult result;
    result.home_locality_key = std::move(home_locality_key);
    result.library_generation = library_generation;
    result.request_id = request_id;
    try {
        result.home_candidates =
            operations.page({}, BackendLibraryFacetKind::City, {}, HOME_CANDIDATE_LIMIT);
        if (result.home_locality_key.isEmpty()) {
            return result;
        }

        BackendLibraryPhotoFilter travel_filter;
        travel_filter.excluded_locality_key = result.home_locality_key;
        result.photo_count = operations.count(travel_filter);
        const BackendLibraryFacetPage countries =
            operations
                .page(travel_filter, BackendLibraryFacetKind::Country, {}, TRAVEL_COUNTRY_LIMIT);
        result.groups.reserve(countries.items.size());
        for (const BackendLibraryFacet& country : countries.items) {
            BackendLibraryPhotoFilter country_filter = travel_filter;
            country_filter.country_key = country.key;
            result.groups.push_back(
                CountryGroup{
                    .country = country,
                    .destinations = operations.page(
                        country_filter,
                        BackendLibraryFacetKind::City,
                        {},
                        TRAVEL_DESTINATION_LIMIT
                    ),
                }
            );
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

QVariantList ReviewTravelCollectionCoordinator::facetVariants(const BackendLibraryFacetPage& page) {
    QVariantList values;
    values.reserve(page.items.size());
    for (const BackendLibraryFacet& item : page.items) {
        values.push_back(
            QVariantMap{
                {QStringLiteral("key"), item.key},
                {QStringLiteral("label"), item.label},
                {
                    QStringLiteral("photoCount"),
                    QVariant::fromValue(static_cast<qulonglong>(item.photo_count)),
                },
            }
        );
    }
    return values;
}

void ReviewTravelCollectionCoordinator::startTask() {
    task_running_ = true;
    active_request_id_ = ++request_id_;
    emit projectionChanged();
    watcher_.setFuture(
        QtConcurrent::run(
            runTask,
            operations_,
            requested_home_locality_key_,
            requested_library_generation_,
            active_request_id_
        )
    );
}

void ReviewTravelCollectionCoordinator::finishTask() {
    TaskResult result = watcher_.result();
    task_running_ = false;
    const bool accepted = result.request_id == active_request_id_
                          && result.library_generation == requested_library_generation_
                          && result.home_locality_key == requested_home_locality_key_;
    if (accepted) {
        home_candidates_ = std::move(result.home_candidates);
        groups_ = std::move(result.groups);
        photo_count_ = result.photo_count;
        error_text_ = std::move(result.error);
    }
    if (refresh_pending_ || !accepted) {
        refresh_pending_ = false;
        startTask();
        return;
    }
    emit projectionChanged();
}
