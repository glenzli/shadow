#include "smart_category_controller.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QThread>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void waitUntilReady(SmartCategoryController& controller) {
    QElapsedTimer timer;
    timer.start();
    while (controller.busy() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(!controller.busy(), "classification must finish");
}

BackendSmartClassificationSnapshot emptySnapshot() {
    BackendSmartClassificationSnapshot snapshot;
    snapshot.status = QStringLiteral("empty");
    return snapshot;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ShadowSmartCategoryTest"));
    QCoreApplication::setApplicationName(QStringLiteral("controller"));
    QSettings().clear();

    BackendSmartClassificationSnapshot snapshot = emptySnapshot();
    QHash<QString, QStringList> members;
    QVector<BackendSmartCategoryReviewItem> review_queue;
    int calls = 0;
    int feedback_calls = 0;
    QString captured_revision;
    QString captured_feedback_category;
    std::int8_t captured_feedback_decision = 0;
    SmartCategoryController controller(
        [&](const QVector<BackendSmartCategoryDefinition>& definitions,
            const QString& config_revision,
            const QString& generation,
            const bool start_new,
            const bool clear_embeddings) {
            require(!definitions.isEmpty(), "default category definitions cross the runner");
            require(!clear_embeddings, "ordinary update preserves embedding cache");
            ++calls;
            captured_revision = config_revision;
            BackendSmartClassificationBatch batch;
            batch.config_revision = config_revision;
            batch.generation = start_new ? QStringLiteral("generation-a") : generation;
            batch.embedding_space = QStringLiteral("siglip-test@build:space:v1");
            batch.model_build = QStringLiteral("test-build");
            batch.total_photos = 2;
            batch.processed_photos = static_cast<std::uint64_t>(calls);
            batch.status = calls == 1 ? QStringLiteral("running") : QStringLiteral("complete");
            if (calls >= 2) {
                snapshot = {
                    .config_revision = config_revision,
                    .generation = batch.generation,
                    .status = QStringLiteral("complete"),
                    .embedding_space = batch.embedding_space,
                    .model_build = batch.model_build,
                    .processed_photos = 2,
                    .total_photos = 2,
                    .has_published_results = true,
                    .published_config_revision = config_revision,
                    .category_counts =
                        {{QStringLiteral("portrait"), 2}, {QStringLiteral("travel"), 1}},
                };
                members.insert(
                    QStringLiteral("portrait"),
                    {QStringLiteral("photo-a\x1frepresentation-a"),
                     QStringLiteral("photo-b\x1frepresentation-b")}
                );
                members.insert(
                    QStringLiteral("travel"),
                    {QStringLiteral("photo-a\x1frepresentation-a")}
                );
                review_queue = {{
                    .photo_id = QStringLiteral("photo-b"),
                    .representation_id = QStringLiteral("representation-b"),
                    .category_id = QStringLiteral("portrait"),
                    .adapted_similarity = 0.071F,
                    .decision_margin = 0.001F,
                }};
            }
            return batch;
        },
        [&]() { return snapshot; },
        [&](const QString& category_id) { return members.value(category_id); },
        [&]() { return review_queue; },
        [&](const QString& photo_id,
            const QString& representation_id,
            const QString& category_id,
            const std::int8_t decision) {
            require(photo_id == QStringLiteral("photo-b"), "feedback preserves the photo identity");
            require(
                representation_id == QStringLiteral("representation-b"),
                "feedback preserves the representation identity"
            );
            ++feedback_calls;
            captured_feedback_category = category_id;
            captured_feedback_decision = decision;
            review_queue.clear();
            snapshot.adaptation_pending = true;
        },
        [&](const QString& generation) {
            snapshot.generation = generation;
            snapshot.status = QStringLiteral("paused");
        }
    );
    require(controller.needsUpdate(), "empty persisted state needs initial classification");
    const QVariantMap first_category = controller.categories().front().toMap();
    require(
        std::abs(first_category.value(QStringLiteral("minimumSimilarity")).toDouble() - 0.07)
            < 0.001,
        "portrait threshold is calibrated to the SigLIP prompt-family range"
    );
    controller.ensureCurrent();
    waitUntilReady(controller);
    require(calls == 2, "controller must continue the durable generation across batches");
    require(controller.progressPercent() == 100, "progress uses persisted cumulative counts");
    require(controller.hasPublishedResults(), "complete generation is published");
    controller.selectCategory(QStringLiteral("portrait"));
    require(
        controller.selectedRepresentationKeys().size() == 2,
        "selected membership is loaded from the persistent index on demand"
    );
    controller.selectCategory(QStringLiteral("travel"));
    require(
        controller.selectedRepresentationKeys().size() == 1,
        "one photo may independently belong to another category"
    );
    require(controller.uncertainCount() == 1, "high-value uncertainty queue is projected");
    require(
        controller.isUncertain(QStringLiteral("photo-b"), QStringLiteral("representation-b")),
        "visible cards can query uncertainty without copying the vector index"
    );
    controller.selectUncertain();
    require(
        controller.selectedRepresentationKeys().size() == 1,
        "uncertainty queue is available through the existing gallery filter"
    );
    controller.recordFeedback(
        QStringLiteral("photo-b"),
        QStringLiteral("representation-b"),
        QStringLiteral("portrait"),
        -1
    );
    require(feedback_calls == 1, "one correction crosses the durable feedback boundary once");
    require(
        captured_feedback_category == QStringLiteral("portrait")
            && captured_feedback_decision == -1,
        "the user's negative decision is not softened before persistence"
    );
    require(
        controller.uncertainCount() == 0,
        "a resolved correction disappears from the review queue immediately"
    );
    require(controller.needsUpdate(), "durable feedback projects the pending adaptation state");

    BackendSmartClassificationSnapshot pending_snapshot = snapshot;
    pending_snapshot.status = QStringLiteral("complete");
    pending_snapshot.config_revision = captured_revision;
    pending_snapshot.generation = QStringLiteral("generation-before-restart");
    pending_snapshot.adaptation_pending = true;
    bool pending_adaptation_resumed = false;
    SmartCategoryController pending_controller(
        [&](const QVector<BackendSmartCategoryDefinition>&,
            const QString& config_revision,
            const QString&,
            const bool start_new,
            const bool clear_embeddings) {
            require(start_new, "a complete published generation starts one adaptation catch-up");
            require(!clear_embeddings, "adaptation catch-up reuses the exact image vector cache");
            pending_adaptation_resumed = true;
            pending_snapshot.adaptation_pending = false;
            pending_snapshot.status = QStringLiteral("complete");
            pending_snapshot.config_revision = config_revision;
            pending_snapshot.generation = QStringLiteral("generation-after-restart");
            return BackendSmartClassificationBatch{
                .config_revision = config_revision,
                .generation = pending_snapshot.generation,
                .status = QStringLiteral("complete"),
                .embedding_space = QStringLiteral("siglip-test@build:space:v1"),
                .model_build = QStringLiteral("test-build"),
                .processed_photos = 2,
                .total_photos = 2,
            };
        },
        [&]() { return pending_snapshot; },
        [](const QString&) { return QStringList{}; },
        []() { return QVector<BackendSmartCategoryReviewItem>{}; },
        [](const QString&, const QString&, const QString&, const std::int8_t) {},
        [](const QString&) {}
    );
    QElapsedTimer pending_timer;
    pending_timer.start();
    while ((!pending_adaptation_resumed || pending_controller.busy())
           && pending_timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(pending_adaptation_resumed, "restart automatically resumes pending adaptation");
    require(
        !pending_controller.needsUpdate(),
        "successful catch-up clears the pending state projected after restart"
    );

    snapshot.adaptation_pending = false;
    snapshot.status = QStringLiteral("running");
    snapshot.config_revision = captured_revision;
    snapshot.generation = QStringLiteral("generation-interrupted");
    snapshot.processed_photos = 1;
    snapshot.total_photos = 2;
    bool resumed = false;
    SmartCategoryController resumed_controller(
        [&](const QVector<BackendSmartCategoryDefinition>&,
            const QString& config_revision,
            const QString& generation,
            const bool start_new,
            const bool) {
            require(!start_new, "resume must keep the persisted generation");
            require(
                generation == QStringLiteral("generation-interrupted"),
                "resume uses the persisted generation identity"
            );
            resumed = true;
            snapshot.status = QStringLiteral("complete");
            snapshot.processed_photos = 2;
            return BackendSmartClassificationBatch{
                .config_revision = config_revision,
                .generation = generation,
                .status = QStringLiteral("complete"),
                .embedding_space = QStringLiteral("siglip-test@build:space:v1"),
                .model_build = QStringLiteral("test-build"),
                .processed_photos = 2,
                .total_photos = 2,
            };
        },
        [&]() { return snapshot; },
        [&](const QString& category_id) { return members.value(category_id); },
        []() { return QVector<BackendSmartCategoryReviewItem>{}; },
        [](const QString&, const QString&, const QString&, const std::int8_t) {},
        [](const QString&) {}
    );
    require(
        resumed_controller.canResume(),
        "a persisted running checkpoint becomes resumable after restart"
    );
    resumed_controller.resume();
    waitUntilReady(resumed_controller);
    require(resumed, "continue action resumes the interrupted job");

    QSettings().clear();
    {
        QSettings previous_settings;
        previous_settings.setValue(QStringLiteral("smartCategories/settingsVersion"), 2);
        previous_settings.beginWriteArray(QStringLiteral("smartCategories/items"), 1);
        previous_settings.setArrayIndex(0);
        previous_settings.setValue(QStringLiteral("id"), QStringLiteral("portrait"));
        previous_settings.setValue(QStringLiteral("name"), QStringLiteral("Portrait"));
        previous_settings.setValue(
            QStringLiteral("description"),
            QStringLiteral("a portrait photograph focused on one or more people")
        );
        previous_settings.setValue(QStringLiteral("minimumSimilarity"), 0.05);
        previous_settings.setValue(QStringLiteral("enabled"), true);
        previous_settings.endArray();
    }
    SmartCategoryController migrated_controller(
        [](const QVector<BackendSmartCategoryDefinition>&,
           const QString&,
           const QString&,
           const bool,
           const bool) { return BackendSmartClassificationBatch{}; },
        []() { return emptySnapshot(); },
        [](const QString&) { return QStringList{}; },
        []() { return QVector<BackendSmartCategoryReviewItem>{}; },
        [](const QString&, const QString&, const QString&, const std::int8_t) {},
        [](const QString&) {}
    );
    require(
        std::abs(
            migrated_controller.categories()
                .front()
                .toMap()
                .value(QStringLiteral("minimumSimilarity"))
                .toDouble()
            - 0.07
        ) < 0.001,
        "untouched preview thresholds migrate to competitive prompt-family floors"
    );

    QSettings().clear();
    std::cout << "smart category controller contract passed\n";
    return EXIT_SUCCESS;
}
