#include "library_query_fixture.hpp"

namespace review_library_query_test {

void run_projection_pagination_contracts() {
    auto state = std::make_shared<QueryBackendState>();
    ReviewModel model;
    ReviewLibraryQueryCoordinator coordinator(operations(state), model);
    QVector<BackendReviewDecisionState> decisions;
    coordinator.setDecisionReconciler(
        [&decisions](BackendReviewDecisionState state) {
            decisions.push_back(std::move(state));
        }
    );
    int query_starts = 0;
    QObject::connect(
        &coordinator,
        &ReviewLibraryQueryCoordinator::queryStarted,
        [&query_starts](
            const BackendLibraryPhotoFilter& filter,
            const quint64 generation
        ) {
            require(
                filter.camera_key == QStringLiteral("paginate")
                    && generation == 2,
                "the reset publishes one immutable filter and generation"
            );
            ++query_starts;
        }
    );

    BackendLibraryPhotoFilter filter;
    filter.camera_key = QStringLiteral("paginate");
    coordinator.requestReset(filter);
    wait_until(
        [&coordinator]() {
            return !coordinator.refreshing()
                && coordinator.itemCount() == 2;
        },
        "the first page and count reach one ready projection"
    );
    require(
        query_starts == 1 && model.rowCount() == 1
            && coordinator.hasMore() && decisions.size() == 1
            && decisions.front().photo_id == QStringLiteral("photo-1")
            && decisions.front().rating == 3,
        "the first page projects its row, decision, count, and continuation"
    );
    require(
        coordinator.loadMore(true),
        "an admitted continuation starts"
    );
    wait_until(
        [&coordinator, &model]() {
            return !coordinator.loadingMore() && model.rowCount() == 2;
        },
        "the continuation appends its distinct row"
    );
    require(
        !coordinator.hasMore() && decisions.size() == 2,
        "the terminal page closes pagination and reconciles its decision"
    );
    {
        std::lock_guard lock(state->mutex);
        require(
            state->page_calls.size() == 2
                && state->page_calls.front().limit == 96
                && state->page_calls.back().cursor_photo_id
                    == QStringLiteral("photo-1"),
            "both page calls share the bound and the append uses the exact cursor"
        );
    }
}

} // namespace review_library_query_test
