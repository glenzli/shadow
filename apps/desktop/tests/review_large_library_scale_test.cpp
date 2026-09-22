#include "justified_review_layout_model.hpp"
#include "review_gallery_grouping_controller.hpp"
#include "review_model.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <cstdlib>
#include <iostream>

#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Review large-library scale contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] ReviewItem photo(const int number) {
    ReviewItem item;
    item.photo_id = QString::number(number);
    item.representation_id = QStringLiteral("representation-") + item.photo_id;
    item.title = QStringLiteral("Photo ") + item.photo_id;
    item.visual_width = number % 5 == 0 ? 3000 : 4500;
    item.visual_height = number % 5 == 0 ? 4500 : 3000;
    item.capture_day = QStringLiteral("2024-%1-%2")
                           .arg(number % 12 + 1, 2, 10, QChar('0'))
                           .arg(number % 28 + 1, 2, 10, QChar('0'));
    item.place_name = QStringLiteral("Place ") + QString::number(number % 6);
    return item;
}

[[nodiscard]] QVector<ReviewItem> photos(const int first, const int count) {
    QVector<ReviewItem> result;
    result.reserve(count);
    for (int number = first; number < first + count; ++number) {
        result.push_back(photo(number));
    }
    return result;
}

[[nodiscard]] qint64 peakRssMiB() {
#if defined(Q_OS_UNIX)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(Q_OS_MACOS)
        return usage.ru_maxrss / (1024 * 1024);
#else
        return usage.ru_maxrss / 1024;
#endif
    }
#endif
    return -1;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    bool parsed = false;
    const int requested = qEnvironmentVariableIntValue("SHADOW_REVIEW_SCALE_PHOTOS", &parsed);
    const int photo_count = parsed ? requested : 10'000;
    require(
        photo_count >= 1'000 && photo_count <= 100'000,
        "the metadata exercise stays within 1000..100000 photos"
    );

    ReviewModel model;
    QElapsedTimer timer;
    timer.start();
    model.replace(photos(0, photo_count), 1);
    const qint64 projection_ms = timer.elapsed();

    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&model);
    timer.restart();
    layout.setAvailableWidth(960);
    const qint64 layout_ms = timer.elapsed();
    require(layout.rowCount() > 0, "initial gallery rows exist");
    require(
        !layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::ItemsRole).toList().isEmpty(),
        "the first visible row materializes on demand"
    );

    int resets = 0;
    QObject::connect(&layout, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    timer.restart();
    require(model.appendSnapshot(photos(photo_count, 96), 1), "an ungrouped page appends");
    QCoreApplication::processEvents();
    const qint64 ungrouped_append_ms = timer.elapsed();
    require(resets == 0, "an ungrouped append preserves visible layout rows");

    ReviewGalleryGroupingController grouping;
    grouping.setSourceModel(&model);
    QObject::connect(
        &grouping,
        &ReviewGalleryGroupingController::sectionsChanged,
        [&layout, &grouping]() { layout.setSections(grouping.sections()); }
    );
    timer.restart();
    grouping.setDimensionSelected(QStringLiteral("date.month"), true);
    grouping.setDimensionSelected(QStringLiteral("place.name"), true);
    const qint64 group_switch_ms = timer.elapsed();
    require(!grouping.sections().isEmpty(), "date and place groups remain navigable");

    timer.restart();
    require(model.appendSnapshot(photos(photo_count + 96, 96), 1), "a grouped page appends");
    QCoreApplication::processEvents();
    const qint64 grouped_append_ms = timer.elapsed();
    require(
        !layout
             .navigationTarget(
                 QString::number(photo_count + 191),
                 QStringLiteral("representation-") + QString::number(photo_count + 191),
                 0,
                 0
             )
             .isEmpty(),
        "the last grouped photo remains reachable"
    );

    std::cout << "Review scale photos=" << photo_count << " projection_ms=" << projection_ms
              << " layout_ms=" << layout_ms << " ungrouped_append96_ms=" << ungrouped_append_ms
              << " group_switch_ms=" << group_switch_ms
              << " grouped_append96_ms=" << grouped_append_ms
              << " groups=" << grouping.sections().size() << " peak_rss_mib=" << peakRssMiB()
              << '\n';
    return EXIT_SUCCESS;
}
