#include "history_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "History coordinator contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate> void wait_until(Predicate predicate, const std::string& message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

BackendPhotoHistoryEntry
photo_entry(const QString& photo_id, const QString& commit_id, const bool working = false) {
    return {
        .commit_id = commit_id,
        .name = photo_id + QStringLiteral(" version"),
        .created_at_ms = 2'000,
        .is_named = true,
        .is_working = working,
        .grade_nodes_modified = 1,
        .changed_parameter_keys = {QStringLiteral("exposure_stops")},
    };
}

struct BackendState final {
    std::mutex mutex;
    std::condition_variable condition;
    bool block_photo_a = false;
    bool photo_a_entered = false;
    bool release_photo_a = false;
    int photo_calls = 0;
};

HistoryCoordinator::Operations operations(const std::shared_ptr<BackendState>& state) {
    return {
        .photo_page =
            [state](const QString& photo_id, const BackendHistoryCursor& cursor, std::uint32_t) {
                std::unique_lock lock(state->mutex);
                ++state->photo_calls;
                if (photo_id == QStringLiteral("photo-a") && state->block_photo_a) {
                    state->photo_a_entered = true;
                    state->condition.notify_all();
                    state->condition.wait(lock, [state]() { return state->release_photo_a; });
                }
                BackendPhotoHistoryPage page;
                if (cursor.empty()) {
                    page.entries = {photo_entry(photo_id, photo_id + QStringLiteral("-2"), true)};
                    page.has_more = true;
                    page.next_cursor = {
                        .created_at_ms = 2'000,
                        .commit_id = photo_id + QStringLiteral("-2"),
                    };
                } else {
                    page.entries = {photo_entry(photo_id, photo_id + QStringLiteral("-1"))};
                }
                return page;
            },
        .library_page =
            [](const BackendHistoryCursor& cursor, std::uint32_t) {
                BackendLibraryHistoryPage page;
                page.entries = {{
                    .commit_id =
                        cursor.empty() ? QStringLiteral("library-2") : QStringLiteral("library-1"),
                    .message = QStringLiteral("Library checkpoint"),
                    .created_at_ms = 2'000,
                    .is_head = cursor.empty(),
                    .photo_changes = 1,
                }};
                page.has_more = cursor.empty();
                page.next_cursor = cursor.empty()
                    ? BackendHistoryCursor{
                        .created_at_ms = 2'000,
                        .commit_id = QStringLiteral("library-2"),
                    }
                    : BackendHistoryCursor{};
                return page;
            },
        .library_ref_page =
            [](const QString& cursor, std::uint32_t) {
                BackendLibraryHistoryRefPage page;
                page.refs = {{
                    .name = cursor.isEmpty() ? QStringLiteral("heads/main")
                                             : QStringLiteral("versions/library-2"),
                    .kind = cursor.isEmpty() ? BackendHistoryRefKind::Branch
                                             : BackendHistoryRefKind::NamedVersion,
                    .commit_id = QStringLiteral("library-2"),
                    .updated_at_ms = 2'000,
                }};
                page.has_more = cursor.isEmpty();
                page.next_cursor = cursor.isEmpty() ? QStringLiteral("heads/main") : QString{};
                return page;
            },
    };
}

QVariant model_value(QAbstractItemModel* model, const int row, const QByteArray& role_name) {
    const int role = model->roleNames().key(role_name, -1);
    require(role >= Qt::UserRole, "requested model role exists");
    return model->data(model->index(row, 0), role);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    auto state = std::make_shared<BackendState>();
    HistoryCoordinator coordinator(operations(state));

    coordinator.openForPhoto(QStringLiteral("photo-a"));
    wait_until(
        [&coordinator]() {
            return !coordinator.photoBusy() && !coordinator.libraryBusy()
                   && !coordinator.libraryRefsBusy();
        },
        "initial photo and Library queries complete"
    );
    require(
        coordinator.photoModel()->rowCount() == 1
            && model_value(coordinator.photoModel(), 0, QByteArrayLiteral("commitId")).toString()
                   == QStringLiteral("photo-a-2")
            && coordinator.photoHasMore(),
        "initial photo page and continuation publish"
    );
    coordinator.loadMorePhoto();
    coordinator.loadMoreLibrary();
    coordinator.loadMoreLibraryRefs();
    wait_until(
        [&coordinator]() {
            return !coordinator.photoBusy() && !coordinator.libraryBusy()
                   && !coordinator.libraryRefsBusy();
        },
        "all continuation pages complete"
    );
    require(
        coordinator.photoModel()->rowCount() == 2 && coordinator.libraryModel()->rowCount() == 2
            && coordinator.libraryRefs().size() == 2,
        "all scopes append stable continuation pages"
    );

    {
        std::lock_guard lock(state->mutex);
        state->block_photo_a = true;
        state->photo_a_entered = false;
        state->release_photo_a = false;
    }
    coordinator.openForPhoto(QStringLiteral("photo-a"));
    {
        std::unique_lock lock(state->mutex);
        state->condition.wait(lock, [state]() { return state->photo_a_entered; });
    }
    coordinator.openForPhoto(QStringLiteral("photo-b"));
    {
        std::lock_guard lock(state->mutex);
        state->release_photo_a = true;
    }
    state->condition.notify_all();
    wait_until(
        [&coordinator]() { return !coordinator.photoBusy(); },
        "replacement photo query completes"
    );
    require(
        coordinator.photoId() == QStringLiteral("photo-b")
            && coordinator.photoModel()->rowCount() == 1
            && model_value(coordinator.photoModel(), 0, QByteArrayLiteral("commitId")).toString()
                   == QStringLiteral("photo-b-2"),
        "late prior-photo result cannot repopulate the current model"
    );
    return EXIT_SUCCESS;
}
