#include "people_scope_controller.hpp"

#include "ai_preferences.hpp"
#include "people_analysis_controller.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <iostream>

namespace {
bool require(bool condition, const char* message) {
    if (!condition)
        std::cerr << "People scope test failed: " << message << '\n';
    return condition;
}

bool waitReady(PeopleScopeController& scope) {
    QElapsedTimer timer;
    timer.start();
    while ((!scope.ready() || scope.busy()) && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return scope.ready() && !scope.busy();
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid())
        return EXIT_FAILURE;
    AiPreferences preferences(
        root.filePath(QStringLiteral("data")),
        root.filePath(QStringLiteral("preferences.ini"))
    );
    BackendPeopleAnalysisReport report;
    report.has_data = true;
    report.groups = {
        BackendPeopleGroup{
            .group_id = QStringLiteral("a"),
            .member_count = 1,
            .photo_ids = {QStringLiteral("photo-a")}
        },
        BackendPeopleGroup{
            .group_id = QStringLiteral("b"),
            .member_count = 1,
            .photo_ids = {QStringLiteral("photo-b")}
        },
    };
    PeopleAnalysisController people(
        PeopleAnalysisController::Operations{
            .load = [report]() { return report; },
        },
        &preferences
    );
    PeopleScopeSnapshot snapshot;
    snapshot.active = true;
    snapshot.filter.album_id = QStringLiteral("event");
    snapshot.excluded_flag = QStringLiteral("all");
    snapshot.excluded_color = QStringLiteral("all");
    const auto main_thread = QThread::currentThreadId();
    bool ran_off_thread = false;
    PeopleScopeController scope(
        [&ran_off_thread, main_thread](const BackendLibraryPhotoFilter& filter) {
            ran_off_thread = QThread::currentThreadId() != main_thread;
            if (filter.album_id == QStringLiteral("event"))
                return filter.photo_ids.contains(QStringLiteral("photo-a")) ? std::uint64_t{1} : 0;
            return filter.photo_ids.contains(QStringLiteral("photo-b")) ? std::uint64_t{1} : 0;
        },
        [](const BackendLibraryPhotoFilter& filter,
           const BackendLibraryPhotoCursor&,
           std::uint32_t) {
            BackendLibraryPhotoPage page;
            BackendReviewItem item;
            item.photo_id = filter.photo_ids.first();
            item.representation_id = QStringLiteral("raw");
            item.source_available = item.photo_id == QStringLiteral("photo-b");
            page.items.append(item);
            return page;
        },
        [&snapshot]() { return snapshot; },
        &people
    );
    scope.setViewActive(true);
    if (!require(waitReady(scope), "scope count completes")
        || !require(ran_off_thread, "catalog count runs off the UI thread")
        || !require(
            scope.counts().value(QStringLiteral("a")).toInt() == 1,
            "first album contains person a"
        )
        || !require(!scope.counts().contains(QStringLiteral("b")), "first album excludes person b"))
        return EXIT_FAILURE;

    snapshot.filter.album_id = QStringLiteral("other");
    scope.invalidate();
    if (!require(waitReady(scope), "changed album recomputes")
        || !require(!scope.counts().contains(QStringLiteral("a")), "old album count is discarded")
        || !require(
            scope.counts().value(QStringLiteral("b")).toInt() == 1,
            "new album contains person b"
        ))
        return EXIT_FAILURE;

    snapshot.only_editable = true;
    scope.invalidate();
    if (!require(waitReady(scope), "client-only filter recomputes")
        || !require(!scope.counts().contains(QStringLiteral("a")), "unavailable source is excluded")
        || !require(
            scope.counts().value(QStringLiteral("b")).toInt() == 1,
            "available source stays visible"
        ))
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
