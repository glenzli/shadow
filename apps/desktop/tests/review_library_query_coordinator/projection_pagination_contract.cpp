#include "library_query_fixture.hpp"

#include <QSet>

namespace review_library_query_test {

void run_projection_pagination_contracts() {
    auto state = std::make_shared<QueryBackendState>();
    ReviewModel model;
    ReviewLibraryQueryCoordinator coordinator(operations(state), model);
    QVector<BackendReviewDecisionState> decisions;
    coordinator.setDecisionReconciler([&decisions](BackendReviewDecisionState state) {
        decisions.push_back(std::move(state));
    });
    int query_starts = 0;
    int initial_page_presentations = 0;
    QObject::connect(
        &coordinator,
        &ReviewLibraryQueryCoordinator::queryStarted,
        [&query_starts](const BackendLibraryPhotoFilter& filter, const quint64 generation) {
            require(
                filter.camera_key == QStringLiteral("paginate") && generation == 2,
                "the reset publishes one immutable filter and generation"
            );
            ++query_starts;
        }
    );
    QObject::connect(
        &coordinator,
        &ReviewLibraryQueryCoordinator::initialPagePresented,
        [&initial_page_presentations](
            const BackendLibraryPhotoFilter& filter,
            const quint64 generation
        ) {
            require(
                filter.camera_key == QStringLiteral("paginate") && generation == 2,
                "the presented page retains its immutable filter and generation"
            );
            ++initial_page_presentations;
        }
    );

    BackendLibraryPhotoFilter filter;
    filter.camera_key = QStringLiteral("paginate");
    coordinator.requestReset(filter, BackendLibraryPhotoOrder::FileNameAscending);
    wait_until(
        [&coordinator]() { return !coordinator.refreshing() && coordinator.itemCount() == 2; },
        "the first page and count reach one ready projection"
    );
    require(
        query_starts == 1 && initial_page_presentations == 1 && model.rowCount() == 1
            && coordinator.hasMore() && decisions.size() == 1
            && decisions.front().photo_id == QStringLiteral("photo-1")
            && decisions.front().rating == 3,
        "the first page projects its row, decision, count, and continuation"
    );
    require(
        model.data(model.index(0, 0), ReviewModel::LocationIdRole).toString()
                == QStringLiteral("photo-1-location")
            && !model.data(model.index(0, 0), ReviewModel::SourceAvailableRole).toBool(),
        "the page projection must preserve the exact location and missing-source state"
    );
    require(coordinator.loadMore(true), "an admitted continuation starts");
    wait_until(
        [&coordinator, &model]() { return !coordinator.loadingMore() && model.rowCount() == 2; },
        "the continuation appends its distinct row"
    );
    require(
        !coordinator.hasMore() && decisions.size() == 2,
        "the terminal page closes pagination and reconciles its decision"
    );
    {
        std::lock_guard lock(state->mutex);
        require(
            state->page_calls.size() == 2 && state->page_calls.front().limit == 96
                && state->page_calls.front().order == BackendLibraryPhotoOrder::FileNameAscending
                && state->page_calls.back().order == BackendLibraryPhotoOrder::FileNameAscending
                && state->page_calls.back().cursor_photo_id == QStringLiteral("photo-1"),
            "both page calls share the bound and the append uses the exact cursor"
        );
    }

    {
        auto live = std::make_shared<QueryBackendState>();
        live->bulk_rows = 300;
        ReviewModel live_model;
        ReviewLibraryQueryCoordinator live_coordinator(operations(live), live_model);
        live_coordinator.setScanRunning(true);
        BackendLibraryPhotoFilter live_filter;
        live_filter.camera_key = QStringLiteral("bulk");
        live_coordinator.requestReset(live_filter, BackendLibraryPhotoOrder::FileNameAscending);
        wait_until([&]() { return !live_coordinator.pageRunning(); }, "first live import page");
        require(
            live_model.rowCount() == 96 && live_coordinator.hasMore(),
            "the live first page retains its continuation"
        );
        {
            std::lock_guard lock(live->mutex);
            live->repeat_cursor_row = true;
        }
        require(live_coordinator.loadMore(true), "paging is admitted during import");
        wait_until(
            [&]() { return !live_coordinator.pageRunning(); },
            "live duplicate-aware append"
        );
        require(
            live_model.rowCount() == 192,
            "a moved cursor row is updated instead of duplicated"
        );
        {
            std::lock_guard lock(live->mutex);
            live->visual_revision = QStringLiteral("-refined");
            live->block_first_page = true;
            live->blocked_page_call = live->page_calls.size() + 1;
        }
        require(
            live_coordinator.refreshStreamingPrefix(true),
            "refresh the complete loaded window"
        );
        wait_until(
            [&]() {
                std::lock_guard lock(live->mutex);
                return live->first_page_entered;
            },
            "streaming refresh enters the backend"
        );
        require(
            !live_coordinator.refreshing() && !live_coordinator.loadingMore(),
            "background visual refresh leaves browsing admission available"
        );
        require(
            live_coordinator.loadMore(true),
            "a browse request queues behind the active refresh"
        );
        {
            std::lock_guard lock(live->mutex);
            live->release_first_page = true;
            live->condition.notify_all();
        }
        wait_until(
            [&]() { return !live_coordinator.pageRunning() && live_model.rowCount() == 288; },
            "the requested continuation runs after the visual refresh"
        );
        require(
            live_model.data(live_model.index(150, 0), ReviewModel::VisualHandleRole)
                .toString()
                .endsWith(QStringLiteral("-refined")),
            "preview upgrades beyond the first page reach already-loaded photos"
        );
        live_coordinator.setScanRunning(false);
        live_coordinator.finishImport();
        wait_until([&]() { return !live_coordinator.pageRunning(); }, "settle the imported window");
        require(
            live_model.rowCount() == 288 && live_coordinator.hasMore(),
            "import completion preserves the amount already browsed"
        );
        require(live_coordinator.loadMore(true), "finish browsing the imported library");
        wait_until([&]() { return !live_coordinator.pageRunning(); }, "last live page");
        QSet<QString> identities;
        for (int row = 0; row < live_model.rowCount(); ++row) {
            identities.insert(
                live_model.data(live_model.index(row, 0), ReviewModel::PhotoIdRole).toString()
            );
        }
        require(
            live_model.rowCount() == 300 && identities.size() == 300 && !live_coordinator.hasMore(),
            "live import paging retains every photo once across upgrade and completion"
        );
    }
}

} // namespace review_library_query_test
