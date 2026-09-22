#include "review_clustering_controller.hpp"

#include <QAbstractListModel>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
bool require(bool condition, const char* message) {
    if (!condition)
        std::cerr << "Review clustering test failed: " << message << '\n';
    return condition;
}

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    QCoreApplication::processEvents();
    return predicate();
}

QVariantList scope() {
    return {
        QVariantMap{
            {QStringLiteral("photoId"), QStringLiteral("a")},
            {QStringLiteral("representationId"), QStringLiteral("ra")}
        },
        QVariantMap{
            {QStringLiteral("photoId"), QStringLiteral("b")},
            {QStringLiteral("representationId"), QStringLiteral("rb")}
        },
        QVariantMap{
            {QStringLiteral("photoId"), QStringLiteral("c")},
            {QStringLiteral("representationId"), QStringLiteral("rc")}
        },
    };
}

class LoadedView final : public QAbstractListModel {
  public:
    int rowCount(const QModelIndex& = {}) const override {
        return 3;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= 3)
            return {};
        if (role == 1)
            return QStringList{QStringLiteral("a"), QStringLiteral("remote"), QStringLiteral("b")}
                .at(index.row());
        if (role == 2)
            return QStringList{QStringLiteral("ra"), QStringLiteral("rr"), QStringLiteral("rb")}.at(
                index.row()
            );
        if (role == 3)
            return index.row() == 1;
        return {};
    }
    QHash<int, QByteArray> roleNames() const override {
        return {{1, "photoId"}, {2, "representationId"}, {3, "isRemote"}};
    }
};
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QSemaphore first_entered;
    QSemaphore release_first;
    std::atomic<int> calls{0};
    std::atomic<std::uint64_t> next_token{0};
    ReviewClusteringController controller(
        [&](const QString& photo_id, const QString& representation_id, std::uint64_t) {
            if (calls.fetch_add(1) == 0) {
                first_entered.release();
                release_first.acquire();
            }
            BackendSemanticSearchReport report;
            report.matches.push_back({photo_id, representation_id, 1.0F});
            if (photo_id == QStringLiteral("a"))
                report.matches.push_back({QStringLiteral("b"), QStringLiteral("rb"), 0.8F});
            if (photo_id == QStringLiteral("b"))
                report.matches.push_back({QStringLiteral("a"), QStringLiteral("ra"), 0.8F});
            return report;
        },
        [&] { return ++next_token; },
        [](std::uint64_t) {},
        {}
    );
    if (!require(controller.startSelected(scope()), "selected scope starts")
        || !require(first_entered.tryAcquire(1, 2000), "work runs off the UI thread")) {
        release_first.release();
        return EXIT_FAILURE;
    }
    controller.pause();
    release_first.release();
    if (!require(waitUntil([&] { return controller.paused(); }), "pause reaches a safe boundary")
        || !require(controller.processed() == 1, "progress retains completed photos")
        || !require(calls.load() == 1, "paused task admits no next inference"))
        return EXIT_FAILURE;
    controller.resume();
    if (!require(waitUntil([&] { return !controller.busy(); }), "resume completes the snapshot")
        || !require(
            controller.processed() == 3 && controller.total() == 3,
            "progress reaches its exact total"
        )
        || !require(
            controller.hasResults() && controller.groups().size() == 1,
            "reciprocal candidates form one review group"
        ))
        return EXIT_FAILURE;
    const auto group = controller.groups()
                           .front()
                           .toMap()
                           .value(QStringLiteral("representationKeys"))
                           .toStringList();
    if (!require(group.size() == 2, "unrelated candidate remains outside the group"))
        return EXIT_FAILURE;

    LoadedView loaded_view;
    if (!require(controller.startView(&loaded_view), "loaded view starts")
        || !require(waitUntil([&] { return !controller.busy(); }), "loaded view completes")
        || !require(
            controller.total() == 2 && controller.hasResults(),
            "remote rows are excluded without aborting local grouping"
        ))
        return EXIT_FAILURE;

    int album_pages = 0;
    ReviewClusteringController album(
        [](const QString& photo_id, const QString& representation_id, std::uint64_t) {
            BackendSemanticSearchReport report;
            report.matches.push_back({photo_id, representation_id, 1.0F});
            return report;
        },
        [&] { return ++next_token; },
        [](std::uint64_t) {},
        [&](const QString& album_id, const BackendLibraryPhotoCursor&, std::uint32_t) {
            if (album_id != QStringLiteral("album-1"))
                throw std::runtime_error("wrong album scope");
            ++album_pages;
            BackendLibraryPhotoPage page;
            page.items.push_back(
                {.photo_id = QStringLiteral("a"),
                 .representation_id = QStringLiteral("ra"),
                 .has_visual = true}
            );
            page.items.push_back(
                {.photo_id = QStringLiteral("b"),
                 .representation_id = QStringLiteral("rb"),
                 .has_visual = true}
            );
            return page;
        }
    );
    if (!require(album.startAlbum(QStringLiteral("album-1")), "album scope starts")
        || !require(waitUntil([&] { return !album.busy(); }), "album scope completes")
        || !require(
            album_pages == 1 && album.total() == 2,
            "album membership is collected off the UI thread"
        ))
        return EXIT_FAILURE;

    QSemaphore cancel_entered;
    QSemaphore release_cancel;
    std::atomic<int> cancelled{0};
    ReviewClusteringController cancellable(
        [&](const QString&, const QString&, std::uint64_t) {
            cancel_entered.release();
            release_cancel.acquire();
            throw std::runtime_error("cancelled provider");
            return BackendSemanticSearchReport{};
        },
        [&] { return ++next_token; },
        [&](std::uint64_t) { ++cancelled; },
        {}
    );
    if (!require(cancellable.startSelected(scope()), "second job starts")
        || !require(cancel_entered.tryAcquire(1, 2000), "provider request begins")) {
        release_cancel.release();
        return EXIT_FAILURE;
    }
    cancellable.cancel();
    release_cancel.release();
    return require(waitUntil([&] { return !cancellable.busy(); }), "cancel terminates")
                   && require(cancelled.load() == 1, "active provider token is cancelled")
                   && require(!cancellable.hasResults(), "cancel publishes no partial groups")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
