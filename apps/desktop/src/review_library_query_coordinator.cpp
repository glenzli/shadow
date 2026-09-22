#include "review_library_query_coordinator.hpp"

#include "review_decision_coordinator.hpp"
#include "review_diagnostics.hpp"

#include <QtConcurrentRun>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t REVIEW_PAGE_SIZE = 96;
constexpr int FILTER_QUERY_DEBOUNCE_MS = 120;

[[nodiscard]] LocalizedUiMessage query_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"ReviewController", source, arguments};
}

[[nodiscard]] bool valid_continuation_cursor(
    const BackendLibraryPhotoOrder order,
    const BackendLibraryPhotoCursor& cursor
) {
    if (cursor.photo_id.isEmpty()) {
        return false;
    }
    switch (order) {
    case BackendLibraryPhotoOrder::CaptureTimeDescending:
    case BackendLibraryPhotoOrder::CaptureTimeAscending:
        return cursor.file_name.isEmpty();
    case BackendLibraryPhotoOrder::FileNameAscending:
    case BackendLibraryPhotoOrder::FileNameDescending:
        return !cursor.has_capture_time && !cursor.file_name.isEmpty();
    }
    return false;
}

} // namespace

ReviewLibraryQueryCoordinator::ReviewLibraryQueryCoordinator(
    Operations operations,
    ReviewModel& model,
    QObject* parent
) : QObject(parent), operations_(std::move(operations)), model_(&model) {
    if (!operations_.page || !operations_.count) {
        throw std::invalid_argument("all Review Library query operations are required");
    }
    model_->replace({}, generation_);
    debounce_timer_.setInterval(FILTER_QUERY_DEBOUNCE_MS);
    debounce_timer_.setSingleShot(true);
    debounce_timer_.setTimerType(Qt::CoarseTimer);
    connect(&debounce_timer_, &QTimer::timeout, this, &ReviewLibraryQueryCoordinator::beginReset);
    connect(
        &page_watcher_,
        &QFutureWatcher<PageTaskResult>::finished,
        this,
        &ReviewLibraryQueryCoordinator::finishPage
    );
    connect(
        &count_watcher_,
        &QFutureWatcher<CountTaskResult>::finished,
        this,
        &ReviewLibraryQueryCoordinator::finishCount
    );
}

ReviewLibraryQueryCoordinator::~ReviewLibraryQueryCoordinator() {
    debounce_timer_.stop();
    page_watcher_.waitForFinished();
    count_watcher_.waitForFinished();
}

bool ReviewLibraryQueryCoordinator::busy() const noexcept {
    return model_->rowCount() == 0 && (scan_running_ || page_running_ || terminal_refresh_active_);
}

bool ReviewLibraryQueryCoordinator::refreshing() const noexcept {
    return page_reset_running_ || terminal_refresh_active_;
}

bool ReviewLibraryQueryCoordinator::loadingMore() const noexcept {
    return page_running_ && !page_reset_running_ && model_->rowCount() > 0;
}

bool ReviewLibraryQueryCoordinator::pageRunning() const noexcept {
    return page_running_;
}

bool ReviewLibraryQueryCoordinator::hasMore() const noexcept {
    return has_more_;
}

int ReviewLibraryQueryCoordinator::itemCount() const noexcept {
    return boundedCount(total_items_);
}

quint64 ReviewLibraryQueryCoordinator::totalItems() const noexcept {
    return total_items_;
}

quint64 ReviewLibraryQueryCoordinator::generation() const noexcept {
    return generation_;
}

LocalizedUiMessage ReviewLibraryQueryCoordinator::statusMessage() const {
    return status_message_;
}

void ReviewLibraryQueryCoordinator::setDecisionReconciler(DecisionReconciler reconciler) {
    decision_reconciler_ = std::move(reconciler);
}

void ReviewLibraryQueryCoordinator::setScanRunning(const bool running) {
    if (scan_running_ == running) {
        return;
    }
    scan_running_ = running;
    emit workStateChanged();
    if (!scan_running_ && !page_running_) {
        requestReadyStatus();
    }
}

void ReviewLibraryQueryCoordinator::setDecisionBusy(const bool busy) {
    if (decision_busy_ == busy) {
        return;
    }
    decision_busy_ = busy;
    if (!decision_busy_ && reset_pending_ && !page_running_ && !debounce_timer_.isActive()) {
        beginReset();
    }
}

void ReviewLibraryQueryCoordinator::clearForImportStart() {
    debounce_timer_.stop();
    reset_pending_ = false;
    terminal_refresh_active_ = false;
    setHasMore(false);
    emit workStateChanged();
}

void ReviewLibraryQueryCoordinator::requestReset(
    BackendLibraryPhotoFilter filter,
    const BackendLibraryPhotoOrder order
) {
    request_clock_.start();
    debounce_timer_.stop();
    requested_filter_ = std::move(filter);
    requested_order_ = order;
    terminal_refresh_active_ = true;
    reset_pending_ = true;
    emit workStateChanged();
    if (page_running_ || decision_busy_) {
        return;
    }
    beginReset();
}

void ReviewLibraryQueryCoordinator::scheduleReset(
    BackendLibraryPhotoFilter filter,
    const BackendLibraryPhotoOrder order
) {
    request_clock_.start();
    requested_filter_ = std::move(filter);
    requested_order_ = order;
    reset_pending_ = true;
    debounce_timer_.start();
}

bool ReviewLibraryQueryCoordinator::loadMore(const bool admitted) {
    if (!admitted || !has_more_ || refreshing() || page_running_ || decision_busy_) {
        return false;
    }
    startPage(PageKind::Append);
    return true;
}

bool ReviewLibraryQueryCoordinator::refreshStreamingPrefix(const bool admitted) {
    if (!admitted || page_running_ || decision_busy_) {
        return false;
    }
    startPage(PageKind::StreamingPrefix);
    return true;
}

ReviewLibraryQueryCoordinator::PageTaskResult ReviewLibraryQueryCoordinator::runPageTask(
    Operations operations,
    BackendLibraryPhotoFilter filter,
    const BackendLibraryPhotoOrder order,
    BackendLibraryPhotoCursor cursor,
    const quint64 generation,
    const quint64 request_id,
    const PageKind kind
) {
    QElapsedTimer clock;
    clock.start();
    PageTaskResult result;
    result.generation = generation;
    result.request_id = request_id;
    result.kind = kind;
    try {
        result.page = operations.page(filter, order, cursor, REVIEW_PAGE_SIZE);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    result.backend_ms = clock.elapsed();
    return result;
}

ReviewLibraryQueryCoordinator::CountTaskResult ReviewLibraryQueryCoordinator::runCountTask(
    Operations operations,
    BackendLibraryPhotoFilter filter,
    const quint64 generation,
    const quint64 request_id
) {
    CountTaskResult result;
    result.generation = generation;
    result.request_id = request_id;
    try {
        result.count = operations.count(filter);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

QVector<ReviewItem> ReviewLibraryQueryCoordinator::reviewItems(QVector<BackendReviewItem> source) {
    QVector<ReviewItem> items;
    items.reserve(source.size());
    for (auto& item : source) {
        items.push_back({
            .photo_id = std::move(item.photo_id),
            .representation_id = std::move(item.representation_id),
            .representation_count = item.representation_count,
            .source_location_count = item.source_location_count,
            .has_raw_representation = item.has_raw_representation,
            .has_raster_representation = item.has_raster_representation,
            .location_id = std::move(item.location_id),
            .visual_handle = std::move(item.visual_handle),
            .decision_head_sequence = item.decision_head_sequence,
            .decision_flag = review_decision_flag_name(item.decision_flag),
            .decision_rating = static_cast<int>(item.decision_rating),
            .liked = item.liked,
            .color_label = std::move(item.color_label),
            .library_state_updated_at_ms = item.library_state_updated_at_ms,
            .has_development_edits = item.has_development_edits,
            .title = std::move(item.title),
            .source_path = std::move(item.source_path),
            .source_available = item.source_available,
            .visual_role = std::move(item.visual_role),
            .visual_width = item.visual_width,
            .visual_height = item.visual_height,
            .has_visual = item.has_visual,
            .has_metadata = item.has_metadata,
            .camera_make = std::move(item.camera_make),
            .camera_model = std::move(item.camera_model),
            .lens_make = std::move(item.lens_make),
            .lens_model = std::move(item.lens_model),
            .captured_at_unix_seconds = item.captured_at_unix_seconds,
            .capture_day = std::move(item.capture_day),
            .place_name = std::move(item.place_name),
            .iso_speed = item.iso_speed,
            .exposure_time_seconds = item.exposure_time_seconds,
            .aperture_f_number = item.aperture_f_number,
            .focal_length_mm = item.focal_length_mm,
            .focal_length_35mm = item.focal_length_35mm,
            .raw_width = item.raw_width,
            .raw_height = item.raw_height,
            .sensor_bits = item.sensor_bits,
            .cfa_pattern = std::move(item.cfa_pattern),
            .dng_version = std::move(item.dng_version),
            .has_technical_observation = item.has_technical_observation,
            .technical_input_width = item.technical_input_width,
            .technical_input_height = item.technical_input_height,
            .technical_preprocessing_version = std::move(item.technical_preprocessing_version),
            .technical_implementation_version = std::move(item.technical_implementation_version),
            .mean_luma = item.mean_luma,
            .p01_luma = item.p01_luma,
            .p50_luma = item.p50_luma,
            .p99_luma = item.p99_luma,
            .near_black_fraction = item.near_black_fraction,
            .near_white_fraction = item.near_white_fraction,
            .laplacian_variance = item.laplacian_variance,
            .edge_energy = item.edge_energy,
        });
    }
    return items;
}

int ReviewLibraryQueryCoordinator::boundedCount(const quint64 count) noexcept {
    return static_cast<int>(
        std::min<quint64>(count, static_cast<quint64>(std::numeric_limits<int>::max()))
    );
}

void ReviewLibraryQueryCoordinator::beginReset() {
    if (page_running_ || decision_busy_) {
        reset_pending_ = true;
        return;
    }
    reset_pending_ = false;
    active_filter_ = requested_filter_;
    active_order_ = requested_order_;
    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
    model_->setGeneration(generation_);
    next_cursor_ = {};
    setHasMore(false);
    if (!scan_running_) {
        terminal_refresh_active_ = true;
    }
    emit queryStarted(active_filter_, generation_);
    startPage(PageKind::InitialReset);
}

void ReviewLibraryQueryCoordinator::startPage(const PageKind kind) {
    if (page_running_ || decision_busy_) {
        return;
    }
    const bool reset = kind != PageKind::Append;
    page_running_ = true;
    page_reset_running_ = reset;
    active_page_request_id_ = ++page_request_id_;
    emit workStateChanged();
    if (reset) {
        emit resetPresentationStarted();
    } else {
        requestReadyStatus();
    }
    page_watcher_.setFuture(
        QtConcurrent::run(
            runPageTask,
            operations_,
            active_filter_,
            active_order_,
            reset ? BackendLibraryPhotoCursor{} : next_cursor_,
            generation_,
            active_page_request_id_,
            kind
        )
    );
}

void ReviewLibraryQueryCoordinator::finishPage() {
    QElapsedTimer projection_clock;
    projection_clock.start();
    PageTaskResult result = page_watcher_.result();
    auto& diagnostics = ReviewDiagnostics::instance();
    diagnostics.record(
        ReviewDiagnostics::Stage::PageBackend,
        result.backend_ms,
        static_cast<int>(result.page.items.size())
    );
    page_running_ = false;
    page_reset_running_ = false;
    const bool accepted = !reset_pending_ && result.generation == generation_
                          && result.request_id == active_page_request_id_;
    const auto continue_pending = [this]() {
        emit workStateChanged();
        if (reset_pending_ && !debounce_timer_.isActive()) {
            beginReset();
        }
    };
    if (!accepted) {
        diagnostics.count(ReviewDiagnostics::Counter::StalePage);
        continue_pending();
        return;
    }

    const auto finish_failure = [this, &result, &continue_pending](const QString& error) {
        ReviewDiagnostics::instance().failure(ReviewDiagnostics::Failure::Page);
        if (result.kind == PageKind::InitialReset && !scan_running_) {
            terminal_refresh_active_ = false;
        }
        publishStatus(query_message(
            scan_running_ ? QT_TRANSLATE_NOOP(
                                "ReviewController",
                                "Live Library refresh delayed · import is still safe and "
                                "continuing · %1"
                            )
                          : QT_TRANSLATE_NOOP(
                                "ReviewController",
                                "Library refresh failed · visible photos retained · %1"
                            ),
            {error}
        ));
        continue_pending();
    };

    if (!result.error.isEmpty()) {
        finish_failure(result.error);
        return;
    }
    if (result.page.has_more
        && (result.page.items.isEmpty()
            || !valid_continuation_cursor(active_order_, result.page.next_cursor))) {
        finish_failure(QStringLiteral("the page exposed an invalid continuation cursor"));
        return;
    }

    QVector<BackendReviewDecisionState> decision_states;
    decision_states.reserve(result.page.items.size());
    try {
        for (const auto& item : result.page.items) {
            if (item.decision_rating > 5) {
                throw std::invalid_argument("Review page contains an invalid decision rating");
            }
            decision_states.push_back({
                .photo_id = item.photo_id,
                .head_sequence = item.decision_head_sequence,
                .flag = item.decision_flag,
                .rating = item.decision_rating,
            });
        }
        auto items = reviewItems(std::move(result.page.items));
        bool projected = false;
        switch (result.kind) {
        case PageKind::InitialReset:
            projected = model_->reconcileSnapshot(std::move(items), generation_);
            break;
        case PageKind::StreamingPrefix:
            projected = model_->reconcilePrefixSnapshot(std::move(items), generation_);
            break;
        case PageKind::Append:
            projected = model_->appendSnapshot(std::move(items), generation_);
            break;
        }
        if (!projected) {
            finish_failure(QStringLiteral("the page contained invalid or duplicate photo ids"));
            return;
        }
    } catch (const std::exception& error) {
        finish_failure(QString::fromUtf8(error.what()));
        return;
    }

    if (decision_reconciler_) {
        for (auto& state : decision_states) {
            decision_reconciler_(std::move(state));
        }
    }
    if (result.kind == PageKind::StreamingPrefix) {
        setHasMore(false);
    } else {
        next_cursor_ = std::move(result.page.next_cursor);
        setHasMore(result.page.has_more);
    }
    if (result.kind == PageKind::InitialReset && !scan_running_) {
        terminal_refresh_active_ = false;
    }
    diagnostics.record(
        ReviewDiagnostics::Stage::PageProjection,
        projection_clock.elapsed(),
        model_->rowCount()
    );
    if (result.kind == PageKind::InitialReset && request_clock_.isValid()) {
        diagnostics.record(
            ReviewDiagnostics::Stage::PageWait,
            request_clock_.elapsed(),
            model_->rowCount()
        );
    }
    emit decisionsReconciled();
    if (reset_pending_ && !debounce_timer_.isActive()) {
        emit workStateChanged();
        beginReset();
        return;
    }
    if (result.kind == PageKind::InitialReset) {
        startCount();
        emit initialPagePresented(active_filter_, generation_);
    }
    requestReadyStatus();
    emit workStateChanged();
}

void ReviewLibraryQueryCoordinator::startCount() {
    if (count_running_) {
        count_pending_ = true;
        return;
    }
    count_running_ = true;
    active_count_request_id_ = ++count_request_id_;
    count_watcher_.setFuture(
        QtConcurrent::run(
            runCountTask,
            operations_,
            active_filter_,
            generation_,
            active_count_request_id_
        )
    );
}

void ReviewLibraryQueryCoordinator::finishCount() {
    const CountTaskResult result = count_watcher_.result();
    count_running_ = false;
    const bool accepted = !reset_pending_ && result.generation == generation_
                          && result.request_id == active_count_request_id_;
    if (accepted && result.error.isEmpty()) {
        if (total_items_ != result.count) {
            total_items_ = result.count;
            emit itemCountChanged();
        }
    } else if (accepted) {
        publishStatus(query_message(
            QT_TRANSLATE_NOOP("ReviewController", "Could not count Library photos · %1"),
            {result.error}
        ));
    }
    if (count_pending_ || !accepted) {
        count_pending_ = false;
        // The next initial page owns Catalog priority and schedules its count.
        // Do not enqueue an obsolete aggregate ahead of pending navigation.
        if (!reset_pending_ && !page_reset_running_) {
            startCount();
        }
        return;
    }
    if (!scan_running_ && !page_running_) {
        requestReadyStatus();
    }
}

void ReviewLibraryQueryCoordinator::setHasMore(const bool has_more) {
    if (has_more_ == has_more) {
        return;
    }
    has_more_ = has_more;
    emit hasMoreChanged();
}

void ReviewLibraryQueryCoordinator::publishStatus(LocalizedUiMessage status) {
    status_message_ = std::move(status);
    emit statusMessageChanged();
}

void ReviewLibraryQueryCoordinator::requestReadyStatus() {
    emit readyStatusRequested();
}
