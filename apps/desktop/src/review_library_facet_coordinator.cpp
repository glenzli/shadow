#include "review_library_facet_coordinator.hpp"

#include <QtConcurrentRun>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t LIBRARY_FACET_PAGE_SIZE = 24;

[[nodiscard]] BackendLibraryPhotoFilter liked_filter() {
    BackendLibraryPhotoFilter filter;
    filter.has_liked = true;
    filter.liked = true;
    return filter;
}

[[nodiscard]] BackendLibraryPhotoFilter five_star_filter() {
    BackendLibraryPhotoFilter filter;
    filter.has_minimum_rating = true;
    filter.minimum_rating = 5;
    return filter;
}

[[nodiscard]] LocalizedUiMessage facet_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

} // namespace

ReviewLibraryFacetCoordinator::ReviewLibraryFacetCoordinator(
    Operations operations,
    QObject* parent
) : QObject(parent), operations_(std::move(operations)) {
    if (!operations_.page) {
        throw std::invalid_argument("the Review Library facet page operation is required");
    }
    if (!operations_.count) {
        throw std::invalid_argument("the Review Library photo count operation is required");
    }
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &ReviewLibraryFacetCoordinator::finishTask
    );
}

ReviewLibraryFacetCoordinator::~ReviewLibraryFacetCoordinator() {
    watcher_.waitForFinished();
}

QVariantList ReviewLibraryFacetCoordinator::captureMonths() const {
    return variants(capture_months_);
}

QVariantList ReviewLibraryFacetCoordinator::cameras() const {
    return variants(cameras_);
}

QVariantList ReviewLibraryFacetCoordinator::lenses() const {
    return variants(lenses_);
}

QVariantList ReviewLibraryFacetCoordinator::countries() const {
    return variants(countries_);
}

QVariantList ReviewLibraryFacetCoordinator::cities() const {
    return variants(cities_);
}

QVariantMap ReviewLibraryFacetCoordinator::systemCollectionCounts() const {
    return {
        {
            QStringLiteral("available"),
            system_collection_counts_available_,
        },
        {
            QStringLiteral("all"),
            QVariant::fromValue(static_cast<qulonglong>(all_photo_count_)),
        },
        {
            QStringLiteral("liked"),
            QVariant::fromValue(static_cast<qulonglong>(liked_photo_count_)),
        },
        {
            QStringLiteral("fiveStar"),
            QVariant::fromValue(static_cast<qulonglong>(five_star_photo_count_)),
        },
    };
}

bool ReviewLibraryFacetCoordinator::busy() const noexcept {
    return task_running_;
}

LocalizedUiMessage ReviewLibraryFacetCoordinator::globalStatusMessage() const {
    return global_status_message_;
}

void ReviewLibraryFacetCoordinator::refresh(
    BackendLibraryPhotoFilter filter,
    const quint64 library_generation
) {
    requested_filter_ = std::move(filter);
    requested_library_generation_ = library_generation;
    if (task_running_) {
        refresh_pending_ = true;
        return;
    }
    startTask();
}

void ReviewLibraryFacetCoordinator::retranslateUi() {
    if (!global_status_message_.isEmpty()) {
        emit globalStatusMessageChanged();
    }
}

QVariantList ReviewLibraryFacetCoordinator::variants(const BackendLibraryFacetPage& page) {
    QVariantList values;
    values.reserve(page.items.size());
    for (const auto& item : page.items) {
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

ReviewLibraryFacetCoordinator::TaskResult ReviewLibraryFacetCoordinator::runTask(
    Operations operations,
    BackendLibraryPhotoFilter filter,
    const quint64 library_generation,
    const quint64 request_id
) {
    TaskResult result;
    result.library_generation = library_generation;
    result.request_id = request_id;
    try {
        result.capture_months =
            operations
                .page(filter, BackendLibraryFacetKind::CaptureMonth, {}, LIBRARY_FACET_PAGE_SIZE);
        result.cameras =
            operations.page(filter, BackendLibraryFacetKind::Camera, {}, LIBRARY_FACET_PAGE_SIZE);
        result.lenses =
            operations.page(filter, BackendLibraryFacetKind::Lens, {}, LIBRARY_FACET_PAGE_SIZE);
        result.countries =
            operations.page(filter, BackendLibraryFacetKind::Country, {}, LIBRARY_FACET_PAGE_SIZE);
        result.cities =
            operations.page(filter, BackendLibraryFacetKind::City, {}, LIBRARY_FACET_PAGE_SIZE);
        result.all_photo_count = operations.count({});
        result.liked_photo_count = operations.count(liked_filter());
        result.five_star_photo_count = operations.count(five_star_filter());
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

void ReviewLibraryFacetCoordinator::startTask() {
    task_running_ = true;
    active_request_id_ = ++request_id_;
    emit facetsChanged();
    watcher_.setFuture(
        QtConcurrent::run(
            runTask,
            operations_,
            requested_filter_,
            requested_library_generation_,
            active_request_id_
        )
    );
}

void ReviewLibraryFacetCoordinator::finishTask() {
    TaskResult result = watcher_.result();
    task_running_ = false;
    const bool accepted = result.library_generation == requested_library_generation_
                          && result.request_id == active_request_id_;
    if (accepted && result.error.isEmpty()) {
        capture_months_ = std::move(result.capture_months);
        cameras_ = std::move(result.cameras);
        lenses_ = std::move(result.lenses);
        countries_ = std::move(result.countries);
        cities_ = std::move(result.cities);
        all_photo_count_ = result.all_photo_count;
        liked_photo_count_ = result.liked_photo_count;
        five_star_photo_count_ = result.five_star_photo_count;
        system_collection_counts_available_ = true;
    } else if (accepted) {
        publishGlobalStatus(facet_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not update Library facets · %1"),
            {result.error}
        ));
    }

    if (refresh_pending_ || !accepted) {
        refresh_pending_ = false;
        startTask();
        return;
    }
    emit facetsChanged();
}

void ReviewLibraryFacetCoordinator::publishGlobalStatus(LocalizedUiMessage status) {
    global_status_message_ = std::move(status);
    emit globalStatusMessageChanged();
}
