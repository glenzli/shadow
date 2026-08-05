#include "review_model.hpp"
#include "review_source_availability_monitor.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "source availability monitor contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] bool source_available(const ReviewModel& model) {
    return model.data(model.index(0, 0), ReviewModel::SourceAvailableRole).toBool();
}

template <typename Predicate> [[nodiscard]] bool wait_until(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2'000) {
        QCoreApplication::processEvents();
        if (predicate()) {
            return true;
        }
        QThread::msleep(5);
    }
    QCoreApplication::processEvents();
    return predicate();
}

void write_source(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "the fixture source must be writable");
    require(file.write("raw") == 3, "the fixture source bytes must be written");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "the temporary source directory must exist");
    const QString source_path = directory.filePath(QStringLiteral("photo.raw"));
    write_source(source_path);

    ReviewItem item;
    item.photo_id = QStringLiteral("photo");
    item.representation_id = QStringLiteral("representation");
    item.location_id = QStringLiteral("location");
    item.source_path = source_path;
    item.source_available = true;

    ReviewModel model;
    model.replace({item}, 1);
    ReviewSourceAvailabilityMonitor monitor(model);

    require(QFile::remove(source_path), "the fixture source must be removable");
    monitor.refreshNow();
    require(
        wait_until([&model]() { return !source_available(model); }),
        "a deleted original must become unavailable without a Library rescan"
    );

    write_source(source_path);
    monitor.refreshNow();
    require(
        wait_until([&model]() { return source_available(model); }),
        "a restored original at the same path must become available again"
    );
    require(QFile::remove(source_path), "the restored fixture must be removable");
    require(
        !monitor.confirmNow({
            .photo_id = item.photo_id,
            .location_id = item.location_id,
            .source_path = item.source_path,
        }),
        "explicit edit admission must synchronously reject a missing original"
    );
    require(
        !source_available(model),
        "explicit edit admission must synchronously update the card state"
    );
    return EXIT_SUCCESS;
}
