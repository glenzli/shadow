#include "image_understanding_controller.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Predicate>
void waitUntil(Predicate predicate, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(predicate(), message);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    require(root.isValid(), "temporary settings root must exist");
    AiPreferences preferences(
        root.path(),
        root.filePath(QStringLiteral("settings.ini"))
    );

    BackendImageUnderstandingSnapshot snapshot;
    int batch_calls = 0;
    bool accepted = false;
    bool dismissed = false;
    ImageUnderstandingController controller(
        &preferences,
        [&](const QString& scope,
            const int minimum_rating,
            const QString& generation,
            const bool start_new,
            const bool auto_apply) {
            require(scope == QStringLiteral("liked"), "default queue scans liked photos");
            require(minimum_rating == 5, "rating preference remains bounded");
            require(!auto_apply, "keyword auto-apply defaults off");
            ++batch_calls;
            BackendImageUnderstandingBatch batch;
            batch.generation = start_new ? QStringLiteral("iu-generation") : generation;
            batch.processed_photos = static_cast<std::uint64_t>(batch_calls);
            batch.total_photos = 2;
            batch.status = batch_calls == 1 ? QStringLiteral("running")
                                            : QStringLiteral("complete");
            snapshot = {
                .available = true,
                .policy_revision =
                    QStringLiteral(
                        "shadow.image-understanding-scan:v1:liked:rating-none:batch-4"
                    ),
                .generation = batch.generation,
                .status = batch.status,
                .processed_photos = batch.processed_photos,
                .total_photos = batch.total_photos,
            };
            return batch;
        },
        [&]() { return snapshot; },
        [&](const QString& generation) {
            snapshot.available = true;
            snapshot.generation = generation;
            snapshot.status = QStringLiteral("paused");
            return snapshot;
        },
        [](const QString& photo_id,
           const QString& representation_id,
           const QString& taxonomy_revision,
           const QVector<BackendClassificationReviewCategory>& categories) {
            require(photo_id == QStringLiteral("photo-a"), "review preserves photo identity");
            require(
                representation_id == QStringLiteral("representation-a"),
                "review preserves representation identity"
            );
            require(
                taxonomy_revision == QStringLiteral("smart-config:test"),
                "review binds the exact taxonomy revision"
            );
            require(categories.size() == 2, "review uses the enabled closed set");
            return BackendClassificationReviewProposal{
                .available = true,
                .photo_id = photo_id,
                .representation_id = representation_id,
                .source_revision = QStringLiteral("shadow:source:1"),
                .taxonomy_revision = taxonomy_revision,
                .disposition = QStringLiteral("matched"),
                .category_id = QStringLiteral("landscape"),
                .proposal_status = QStringLiteral("suggested"),
                .model_profile = QStringLiteral("general"),
                .model_build = QStringLiteral("qwen-8b-test"),
            };
        },
        [&](const QString& photo_id,
            const QString& representation_id,
            const QString& source_revision) {
            require(photo_id == QStringLiteral("photo-a"), "accept preserves photo identity");
            require(
                representation_id == QStringLiteral("representation-a"),
                "accept preserves representation identity"
            );
            require(
                source_revision == QStringLiteral("shadow:source:1"),
                "accept compares the exact visual revision"
            );
            accepted = true;
            return QStringLiteral("landscape");
        },
        [&](const QString&, const QString&, const QString&) { dismissed = true; },
        [](const QString& photo_id, const QString& representation_id) {
            return BackendImageUnderstandingProposal{
                .available = true,
                .photo_id = photo_id,
                .representation_id = representation_id,
                .source_revision = QStringLiteral("shadow:description:1"),
                .description = QStringLiteral("A mountain at sunset."),
                .language = QStringLiteral("en"),
                .keywords = {QStringLiteral("mountain"), QStringLiteral("sunset")},
                .disposition = QStringLiteral("suggested"),
            };
        },
        [](const QString&, const QString&, const QString&) {},
        []() {
            return QVector<BackendClassificationReviewCategory>{
                {QStringLiteral("portrait"), QStringLiteral("Portrait"), QStringLiteral("people")},
                {
                    QStringLiteral("landscape"),
                    QStringLiteral("Landscape"),
                    QStringLiteral("outdoor scene"),
                },
            };
        },
        []() { return QStringLiteral("smart-config:test"); }
    );

    require(!controller.busy(), "background analysis defaults off");
    preferences.setImageUnderstandingBackgroundEnabled(true);
    waitUntil(
        [&]() { return batch_calls == 1 && controller.canPause(); },
        "queue must expose a checkpointable running boundary"
    );
    controller.pause();
    waitUntil(
        [&]() { return controller.canResume(); },
        "paused checkpoint must be resumable after the active batch"
    );
    require(
        snapshot.status == QStringLiteral("paused"),
        "pause must persist the current generation"
    );
    controller.resume();
    waitUntil([&]() { return batch_calls == 2 && !controller.busy(); }, "queue must complete");
    require(controller.progressPercent() == 100, "progress is cumulative across batches");
    controller.loadPhotoProposal(
        QStringLiteral("photo-a"),
        QStringLiteral("representation-a")
    );
    require(controller.photoProposalAvailable(), "durable description proposal is projected");
    require(controller.photoKeywords().size() == 2, "bounded keyword suggestions cross Qt");

    controller.requestAdvancedReview(
        QStringLiteral("photo-a"),
        QStringLiteral("representation-a")
    );
    waitUntil(
        [&]() {
            return !controller.advancedReviewBusy()
                && controller.advancedReviewDisposition() == QStringLiteral("matched");
        },
        "advanced review must finish"
    );
    require(
        controller.advancedReviewDisposition() == QStringLiteral("matched"),
        "matched review is projected as assistant evidence"
    );
    require(
        controller.advancedReviewCategoryName() == QStringLiteral("Landscape"),
        "category identity resolves through the user taxonomy"
    );
    controller.acceptAdvancedReview();
    require(accepted, "explicit acceptance crosses the adaptation boundary");
    require(!dismissed, "acceptance does not dismiss the proposal");

    std::cout << "image understanding controller contract passed\n";
    return EXIT_SUCCESS;
}
