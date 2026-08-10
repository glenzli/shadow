#include "semantic_search_controller.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>

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
    SemanticSearchController controller(
        [&received_query,
         &received_revision](const QString& query, const QString& revision, const QString&) {
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
        }
    );
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
        || !require(keys.size() == 2, "weak semantic matches are hidden")
        || !require(controller.highRelevanceCount() == 1, "top score is highly relevant")
        || !require(
            controller.possibleRelevanceCount() == 1,
            "the middle score band is kept as possible"
        )
        || !require(
            controller.hiddenLowRelevanceCount() == 2,
            "low relevance candidates are counted but not shown"
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
    if (!require(high_keys.size() == 1, "the strongest section has one exact member")
        || !require(possible_keys.size() == 1, "the possible section has one exact member")
        || !require(
            possible_keys.front()
                == QStringLiteral("photo-a") + QChar{0x001f} + QStringLiteral("representation-a"),
            "possible relevance keeps provider order"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            controller.shownResultCount() == 2,
            "both visible sections remain in the gallery"
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

    SemanticSearchController failing(
        [](const QString&, const QString&, const QString&) -> BackendSemanticSearchReport {
            throw std::runtime_error("credential leaked detail");
        }
    );
    failing.search(QStringLiteral("portrait"));
    waitForCompletion(failing);
    return require(!failing.errorText().isEmpty(), "provider failure becomes a safe UI error")
                   && require(
                       !failing.errorText().contains(QStringLiteral("credential leaked detail")),
                       "raw provider diagnostics are not exposed to QML"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
