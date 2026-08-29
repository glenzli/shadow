#include "people_analysis_controller.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "People analysis controller test failed: " << message << '\n';
    }
    return condition;
}

void waitForCompletion(PeopleAnalysisController& controller) {
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
    PeopleAnalysisController controller([] {
        return BackendPeopleAnalysisReport{
            .analyzed_photos = 12,
            .detected_faces = 8,
            .embedded_faces = 7,
            .skipped_items = 2,
            .ungrouped_faces = 1,
            .truncated = false,
            .groups = {
                {
                    .group_id = QStringLiteral("group-a"),
                    .member_count = 2,
                    .photo_ids = {QStringLiteral("photo-1"), QStringLiteral("photo-2")},
                    .thumbnail_jpeg = QByteArrayLiteral("jpeg-a"),
                },
                {
                    .group_id = QStringLiteral("group-b"),
                    .member_count = 2,
                    .photo_ids = {QStringLiteral("photo-3"), QStringLiteral("photo-4")},
                    .thumbnail_jpeg = QByteArrayLiteral("jpeg-b"),
                },
                {
                    .group_id = QStringLiteral("group-c"),
                    .member_count = 2,
                    .photo_ids = {QStringLiteral("photo-2"), QStringLiteral("photo-5")},
                    .thumbnail_jpeg = QByteArrayLiteral("jpeg-c"),
                },
            },
        };
    });
    controller.startAnalysis();
    waitForCompletion(controller);
    const QVariantList groups = controller.groups();
    if (!require(!controller.busy(), "analysis reaches a terminal state")
        || !require(controller.hasResults(), "successful analysis publishes session results")
        || !require(groups.size() == 3, "anonymous groups reach the QML projection")
        || !require(
            groups.front().toMap().value(QStringLiteral("photoCount")).toUInt() == 2,
            "group photo count is preserved"
        )
        || !require(
            groups.front()
                .toMap()
                .value(QStringLiteral("thumbnailSource"))
                .toString()
                .startsWith(QStringLiteral("data:image/jpeg;base64,")),
            "representative face thumbnail reaches the QML projection"
        )
        || !require(controller.analyzedPhotos() == 12, "summary counts are preserved")) {
        return EXIT_FAILURE;
    }

    controller.toggleGroupSelection(QStringLiteral("group-a"));
    controller.toggleGroupSelection(QStringLiteral("group-c"));
    if (!require(controller.selectedGroupCount() == 2, "two groups can be selected")
        || !require(
            !controller.canMergeSelectedGroups(),
            "groups with a co-occurring photo cannot be merged"
        )) {
        return EXIT_FAILURE;
    }
    controller.toggleGroupSelection(QStringLiteral("group-c"));
    controller.toggleGroupSelection(QStringLiteral("group-b"));
    if (!require(controller.canMergeSelectedGroups(), "disjoint groups can be merged")) {
        return EXIT_FAILURE;
    }
    controller.mergeSelectedGroups();
    const QVariantList merged_groups = controller.groups();
    if (!require(merged_groups.size() == 2, "merge replaces selected groups with one group")
        || !require(
            merged_groups.front().toMap().value(QStringLiteral("photoCount")).toUInt() == 4,
            "merged group carries every selected photo"
        )
        || !require(
            merged_groups.front().toMap().value(QStringLiteral("merged")).toBool(),
            "merged group is marked as a session correction"
        )
        || !require(controller.canUndoMerge(), "merge exposes one-step undo")) {
        return EXIT_FAILURE;
    }
    controller.undoLastMerge();
    if (!require(controller.groups().size() == 3, "undo restores the prior grouping")
        || !require(!controller.canUndoMerge(), "undo is consumed after restoration")) {
        return EXIT_FAILURE;
    }

    controller.clearSessionResults();
    if (!require(!controller.hasResults(), "clear removes only session results")
        || !require(controller.groups().isEmpty(), "clear removes projected groups")) {
        return EXIT_FAILURE;
    }

    PeopleAnalysisController failing([]() -> BackendPeopleAnalysisReport {
        throw std::runtime_error("provider unavailable");
    });
    failing.startAnalysis();
    waitForCompletion(failing);
    return require(!failing.errorText().isEmpty(), "provider failure becomes a safe UI error")
                   && require(
                       !failing.errorText().contains(QStringLiteral("provider unavailable")),
                       "raw provider diagnostics are not exposed to QML"
                   )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
