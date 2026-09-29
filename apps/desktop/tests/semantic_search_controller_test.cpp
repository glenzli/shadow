#include "semantic_search_controller.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <QSemaphore>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Semantic search controller test failed: " << message << '\n';
    }
    return condition;
}

void waitForCompletion(SemanticSearchController& controller) {
    QElapsedTimer timer;
    timer.start();
    while (controller.busy() && timer.elapsed() < 2'000) {
        QCoreApplication::processEvents();
    }
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QString received_query;
    QString received_revision;
    SemanticSearchController controller([&received_query, &received_revision](
                                            const QString& query,
                                            const QString& revision,
                                            const QString&,
                                            std::uint64_t
                                        ) {
        received_query = query;
        received_revision = revision;
        return BackendSemanticSearchReport{
            .considered_photos = 12,
            .embedded_photos = 2,
            .skipped_items = 1,
            .truncated = false,
            .matches = {
                {
                    .photo_id = QStringLiteral("photo-b"),
                    .representation_id = QStringLiteral("representation-b"),
                    .cosine_similarity = 0.81F,
                },
                {
                    .photo_id = QStringLiteral("photo-a"),
                    .representation_id = QStringLiteral("representation-a"),
                    .cosine_similarity = 0.77F,
                },
                {
                    .photo_id = QStringLiteral("photo-c"),
                    .representation_id = QStringLiteral("representation-c"),
                    .cosine_similarity = 0.70F,
                },
                {
                    .photo_id = QStringLiteral("photo-d"),
                    .representation_id = QStringLiteral("representation-d"),
                    .cosine_similarity = 0.62F,
                },
            },
        };
    });
    controller.search(QStringLiteral("  seaside sunset  "));
    waitForCompletion(controller);
    const QStringList keys = controller.rankedRepresentationKeys();
    if (!require(!controller.busy(), "search reaches a terminal state")
        || !require(controller.hasResults(), "successful search publishes session results")
        || !require(received_query == QStringLiteral("seaside sunset"), "query is trimmed")
        || !require(
            received_revision.startsWith(QStringLiteral("shadow:semantic-search-ui/query:")),
            "a stable path-free query revision is generated"
        )
        || !require(keys.size() == 4, "all ranked candidates remain visible")
        || !require(
            controller.highRelevanceCount() == 0,
            "similarity is not presented as confidence"
        )
        || !require(
            controller.possibleRelevanceCount() == 4,
            "all candidates remain in the relative ranking"
        )
        || !require(
            controller.hiddenLowRelevanceCount() == 0,
            "candidates are never hidden by uncalibrated thresholds"
        )
        || !require(
            keys.front()
                == QStringLiteral("photo-b") + QChar{0x001f} + QStringLiteral("representation-b"),
            "provider ranking is preserved"
        )) {
        return EXIT_FAILURE;
    }

    const QStringList high_keys = controller.highRepresentationKeys();
    const QStringList possible_keys = controller.possibleRepresentationKeys();
    if (!require(high_keys.isEmpty(), "no section claims calibrated strong relevance")
        || !require(possible_keys.size() == 4, "the ranking section retains all candidates")
        || !require(
            possible_keys.front()
                == QStringLiteral("photo-b") + QChar{0x001f} + QStringLiteral("representation-b"),
            "possible relevance keeps provider order"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            controller.shownResultCount() == 4,
            "all ranked candidates remain in the gallery"
        )) {
        return EXIT_FAILURE;
    }

    controller.clearSessionResults();
    if (!require(!controller.hasResults(), "clear removes session ranking")
        || !require(
            controller.rankedRepresentationKeys().isEmpty(),
            "clear releases the Review filter"
        )
        || !require(controller.highRepresentationKeys().isEmpty(), "clear removes section members")
        || !require(
            controller.possibleRepresentationKeys().isEmpty(),
            "clear removes broader members"
        )) {
        return EXIT_FAILURE;
    }

    // The old worker is allowed to complete after cancellation: its result must
    // never return to the gallery, and only the latest queued query may run.
    QSemaphore entered;
    QSemaphore release;
    std::atomic<int> calls{0};
    int cancellations = 0;
    SemanticSearchController cancellable(
        [&](const QString&, const QString&, const QString&, std::uint64_t) {
            if (calls.fetch_add(1) == 0) {
                entered.release();
                release.acquire();
            }
            return BackendSemanticSearchReport{};
        },
        {},
        [&](std::uint64_t) { ++cancellations; }
    );
    cancellable.search(QStringLiteral("first"));
    if (!entered.tryAcquire(1, 2000)) {
        release.release();
        return EXIT_FAILURE;
    }
    cancellable.search(QStringLiteral("obsolete"));
    cancellable.search(QStringLiteral("latest"));
    release.release();
    waitForCompletion(cancellable);
    if (!require(cancellations == 2 && calls == 2, "latest query replaces pending work")
        || !require(
            cancellable.activeQuery() == QStringLiteral("latest"),
            "cancelled result is discarded"
        )) {
        return EXIT_FAILURE;
    }
    cancellable.clearSessionResults();
    cancellable.search(QStringLiteral("clear before delivery"));
    cancellable.clearSessionResults();
    waitForCompletion(cancellable);
    if (!require(!cancellable.hasResults(), "late result cannot undo clear"))
        return EXIT_FAILURE;

    SemanticSearchController failing(
        [](const QString&, const QString&, const QString&, std::uint64_t)
            -> BackendSemanticSearchReport { throw std::runtime_error("credential leaked detail"); }
    );
    failing.search(QStringLiteral("portrait"));
    waitForCompletion(failing);
    SemanticSearchController similar(
        SemanticSearchController::ImageRunner{
            [](const QString& photo_id, const QString& representation_id, std::uint64_t) {
                if (photo_id != QStringLiteral("anchor")
                    || representation_id != QStringLiteral("representation-anchor")) {
                    throw std::runtime_error("unexpected anchor");
                }
                return BackendSemanticSearchReport{
                    .considered_photos = 2,
                    .embedded_photos = 2,
                    .matches = {
                        {
                            .photo_id = QStringLiteral("anchor"),
                            .representation_id = QStringLiteral("representation-anchor"),
                            .cosine_similarity = 1.0F,
                        },
                        {
                            .photo_id = QStringLiteral("neighbor"),
                            .representation_id = QStringLiteral("representation-neighbor"),
                            .cosine_similarity = 0.8F,
                        },
                    },
                };
            }
        },
        {},
        {}
    );
    similar.findSimilar(QStringLiteral("anchor"), QStringLiteral("representation-anchor"));
    waitForCompletion(similar);
    if (!require(similar.hasResults(), "image search publishes transient candidates")
        || !require(
            similar.anchorPhotoId() == QStringLiteral("anchor")
                && similar.anchorRepresentationId() == QStringLiteral("representation-anchor"),
            "image results retain the exact anchor identity"
        )
        || !require(
            similar.rankedRepresentationKeys().size() == 2,
            "anchor and neighbor remain visible"
        )) {
        return EXIT_FAILURE;
    }
    similar.clearSessionResults();
    if (!require(
            similar.anchorPhotoId().isEmpty() && !similar.hasResults(),
            "clear removes the image session identity"
        )) {
        return EXIT_FAILURE;
    }

    QSemaphore similar_entered;
    QSemaphore release_similar;
    int similar_cancellations = 0;
    SemanticSearchController cancellable_similar(
        SemanticSearchController::ImageRunner{
            [&](const QString&, const QString&, std::uint64_t) {
                similar_entered.release();
                release_similar.acquire();
                return BackendSemanticSearchReport{};
            }
        },
        [] { return std::uint64_t{1}; },
        [&](std::uint64_t) { ++similar_cancellations; }
    );
    cancellable_similar.findSimilar(QStringLiteral("a"), QStringLiteral("ra"));
    if (!similar_entered.tryAcquire(1, 2000)) {
        release_similar.release();
        return EXIT_FAILURE;
    }
    cancellable_similar.clearSessionResults();
    if (!require(cancellable_similar.busy(), "cancel remains in flight until worker exits")
        || !require(cancellable_similar.statusText().contains(QStringLiteral("Cancelling")),
                    "image review exposes its cancellation state")) {
        release_similar.release();
        return EXIT_FAILURE;
    }
    release_similar.release();
    waitForCompletion(cancellable_similar);
    if (!require(similar_cancellations == 1 && !cancellable_similar.hasResults(),
                 "cancelled image review rejects late results"))
        return EXIT_FAILURE;

    return require(!failing.errorText().isEmpty(), "provider failure becomes a safe UI error")
                   && require(
                       !failing.errorText().contains(QStringLiteral("credential leaked detail")),
                       "raw provider diagnostics are not exposed to QML"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
