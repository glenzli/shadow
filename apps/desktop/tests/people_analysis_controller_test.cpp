#include "people_analysis_controller.hpp"

#include "ai_preferences.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QVariantMap>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

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

PeopleAnalysisController::Operations operationsForReport(BackendPeopleAnalysisReport report) {
    auto current = std::make_shared<BackendPeopleAnalysisReport>();
    auto undo = std::make_shared<BackendPeopleAnalysisReport>();
    report.has_data = true;
    return {
        .load = [current] { return *current; },
        .begin =
            [](const bool authorized) {
                if (!authorized) {
                    throw std::runtime_error("authorization required");
                }
                return std::uint64_t{41};
            },
        .execute =
            [current,
             report = std::move(report)](const std::uint64_t token, const bool authorized) {
                if (!authorized) {
                    throw std::runtime_error("authorization required");
                }
                *current = report;
                return BackendPeopleAnalysisExecution{
                    .job_token = token,
                    .report = *current,
                };
            },
        .progress =
            [](const std::uint64_t token) {
                return BackendPeopleAnalysisProgress{
                    .job_token = token,
                    .phase = QStringLiteral("reviewing"),
                    .maximum_photos = 512,
                };
            },
        .cancel = [](std::uint64_t) { return true; },
        .retire = [](std::uint64_t) {},
        .merge =
            [current, undo](const QStringList& person_ids) {
                *undo = *current;
                BackendPeopleGroup merged;
                for (const BackendPeopleGroup& group : std::as_const(current->groups)) {
                    if (!person_ids.contains(group.group_id)) {
                        continue;
                    }
                    if (merged.group_id.isEmpty()) {
                        merged.group_id = group.group_id;
                        merged.display_name = group.display_name;
                        merged.thumbnail_jpeg = group.thumbnail_jpeg;
                    }
                    merged.member_count += group.member_count;
                    merged.photo_ids.append(group.photo_ids);
                }
                merged.manually_merged = true;
                current->groups.erase(
                    std::remove_if(
                        current->groups.begin(),
                        current->groups.end(),
                        [&person_ids](const BackendPeopleGroup& group) {
                            return person_ids.contains(group.group_id);
                        }
                    ),
                    current->groups.end()
                );
                current->groups.prepend(std::move(merged));
                current->can_undo_merge = true;
                return *current;
            },
        .rename =
            [current](const QString& person_id, const QString& display_name) {
                const auto found = std::find_if(
                    current->groups.begin(),
                    current->groups.end(),
                    [&person_id](const BackendPeopleGroup& group) {
                        return group.group_id == person_id;
                    }
                );
                if (found == current->groups.end()) {
                    throw std::runtime_error("person unavailable");
                }
                found->display_name = display_name.trimmed();
                current->can_undo_merge = false;
                return *current;
            },
        .undo_merge =
            [current, undo] {
                *current = *undo;
                current->can_undo_merge = false;
                return *current;
            },
        .clear = [current] { *current = {}; },
    };
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) {
        return EXIT_FAILURE;
    }
    AiPreferences preferences(
        root.filePath(QStringLiteral("application-data")),
        root.filePath(QStringLiteral("preferences.ini"))
    );
    preferences.grantPeopleAnalysisConsent();
    PeopleAnalysisController controller(
        operationsForReport(
            BackendPeopleAnalysisReport{
                .analyzed_photos = 12,
                .detected_faces = 8,
                .embedded_faces = 7,
                .skipped_items = 2,
                .ungrouped_faces = 1,
                .truncated = false,
                .groups =
                    {
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
            }
        ),
        &preferences
    );
    controller.startAnalysis();
    waitForCompletion(controller);
    const QVariantList groups = controller.groups();
    if (!require(!controller.busy(), "analysis reaches a terminal state")
        || !require(controller.hasResults(), "successful analysis publishes stored people data")
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

    controller.renameGroup(QStringLiteral("group-a"), QStringLiteral("  Alice  "));
    if (!require(
            controller.groups().front().toMap().value(QStringLiteral("displayName")).toString()
                == QStringLiteral("Alice"),
            "a local person name reaches the QML projection"
        )) {
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
        || !require(
            merged_groups.front().toMap().value(QStringLiteral("displayName")).toString()
                == QStringLiteral("Alice"),
            "merge preserves the target person's local name"
        )
        || !require(controller.canUndoMerge(), "merge exposes one-step undo")) {
        return EXIT_FAILURE;
    }
    controller.undoLastMerge();
    if (!require(controller.groups().size() == 3, "undo restores the prior grouping")
        || !require(!controller.canUndoMerge(), "undo is consumed after restoration")) {
        return EXIT_FAILURE;
    }

    controller.clearPeopleData();
    if (!require(!controller.hasResults(), "clear removes stored people results")
        || !require(controller.groups().isEmpty(), "clear removes projected groups")) {
        return EXIT_FAILURE;
    }

    PeopleAnalysisController failing(
        {
            .load = [] { return BackendPeopleAnalysisReport{}; },
            .begin = [](bool) { return std::uint64_t{42}; },
            .execute = [](std::uint64_t, bool) -> BackendPeopleAnalysisExecution {
                throw std::runtime_error("provider unavailable");
            },
            .progress = [](
                            const std::uint64_t token
                        ) { return BackendPeopleAnalysisProgress{.job_token = token}; },
            .cancel = [](std::uint64_t) { return true; },
            .retire = [](std::uint64_t) {},
            .merge = [](const QStringList&) { return BackendPeopleAnalysisReport{}; },
            .rename = [](const QString&, const QString&) { return BackendPeopleAnalysisReport{}; },
            .undo_merge = [] { return BackendPeopleAnalysisReport{}; },
            .clear = [] {},
        },
        &preferences
    );
    failing.startAnalysis();
    waitForCompletion(failing);
    if (!require(!failing.errorText().isEmpty(), "provider failure becomes a safe UI error")
        || !require(
            !failing.errorText().contains(QStringLiteral("provider unavailable")),
            "raw provider diagnostics are not exposed to QML"
        )) {
        return EXIT_FAILURE;
    }

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto retired = std::make_shared<std::atomic_bool>(false);
    PeopleAnalysisController cancellable(
        {
            .load = [] { return BackendPeopleAnalysisReport{}; },
            .begin = [](bool) { return std::uint64_t{43}; },
            .execute =
                [cancelled](const std::uint64_t token, bool) {
                    while (!cancelled->load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    return BackendPeopleAnalysisExecution{
                        .job_token = token,
                        .cancelled = true,
                    };
                },
            .progress =
                [](const std::uint64_t token) {
                    return BackendPeopleAnalysisProgress{
                        .job_token = token,
                        .phase = QStringLiteral("reviewing"),
                        .analyzed_photos = 3,
                        .maximum_photos = 512,
                        .detected_faces = 2,
                        .compared_faces = 1,
                    };
                },
            .cancel =
                [cancelled](std::uint64_t) {
                    cancelled->store(true, std::memory_order_release);
                    return true;
                },
            .retire = [retired](std::uint64_t) { retired->store(true, std::memory_order_release); },
            .merge = [](const QStringList&) { return BackendPeopleAnalysisReport{}; },
            .rename = [](const QString&, const QString&) { return BackendPeopleAnalysisReport{}; },
            .undo_merge = [] { return BackendPeopleAnalysisReport{}; },
            .clear = [] {},
        },
        &preferences
    );
    auto batches = std::make_shared<std::atomic_int>(0);
    auto incremental_operations = operationsForReport({});
    incremental_operations.execute = [batches](std::uint64_t token, bool) {
        const int batch = ++*batches;
        BackendPeopleAnalysisReport report;
        report.has_data = true;
        report.analyzed_photos = std::uint32_t(batch * 512);
        report.truncated = batch == 1;
        return BackendPeopleAnalysisExecution{
            .made_progress = true,
            .job_token = token,
            .report = report
        };
    };
    PeopleAnalysisController incremental(std::move(incremental_operations), &preferences);
    incremental.startAnalysis();
    waitForCompletion(incremental);
    if (!require(
            *batches == 2 && !incremental.busy(),
            "saved incremental batches automatically continue"
        ))
        return EXIT_FAILURE;

    cancellable.startAnalysis();
    cancellable.cancelAnalysis();
    waitForCompletion(cancellable);
    return require(!cancellable.busy(), "cancelled analysis reaches a terminal state")
                   && require(
                       cancellable.statusText()
                           .contains(QStringLiteral("stopped"), Qt::CaseInsensitive),
                       "cancelled analysis has a distinct non-error status"
                   )
                   && require(cancellable.errorText().isEmpty(), "cancellation is not a failure")
                   && require(retired->load(std::memory_order_acquire), "terminal job is retired")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
