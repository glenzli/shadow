#include "justified_review_layout_model.hpp"
#include "review_model.hpp"

#include <QSortFilterProxyModel>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "justified review layout contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] ReviewItem
photo(const char* const id, const std::uint32_t width, const std::uint32_t height) {
    ReviewItem item;
    item.photo_id = QString::fromLatin1(id);
    item.representation_id = QStringLiteral("representation-") + item.photo_id;
    item.title = item.photo_id;
    item.visual_width = width;
    item.visual_height = height;
    return item;
}

void preserves_aspect_ratio_and_never_crops() {
    ReviewModel photos;
    photos.replace(
        {
            photo("landscape", 6000, 4000),
            photo("portrait", 4000, 6000),
            photo("square", 4000, 4000),
        },
        1
    );

    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&photos);
    layout.setAvailableWidth(900);
    layout.setTargetRowHeight(180);

    require(layout.rowCount() >= 1, "photos must produce visible rows");
    const QVariantList first_row =
        layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::ItemsRole).toList();
    require(!first_row.isEmpty(), "a row must expose its photo maps");

    for (const QVariant& variant : first_row) {
        const QVariantMap item = variant.toMap();
        const qreal expected = item.value(QStringLiteral("visualWidth")).toReal()
                               / item.value(QStringLiteral("visualHeight")).toReal();
        const qreal actual =
            item.value(QStringLiteral("layoutWidth")).toReal()
            / layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::RowHeightRole).toReal();
        require(
            std::abs(expected - actual) < 0.02,
            "each displayed thumbnail must retain the original aspect ratio"
        );
    }
}

void density_changes_the_target_thumbnail_scale() {
    ReviewModel photos;
    photos.replace({photo("portrait", 3000, 4500)}, 1);

    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&photos);
    layout.setAvailableWidth(1000);
    layout.setTargetRowHeight(120);
    const int small_height =
        layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::RowHeightRole).toInt();
    layout.setTargetRowHeight(280);
    const int large_height =
        layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::RowHeightRole).toInt();
    require(
        large_height > small_height,
        "the toolbar density control must enlarge an incomplete final row"
    );
}

void keyboard_navigation_follows_rows_and_nearest_columns() {
    ReviewModel photos;
    photos.replace(
        {
            photo("a", 1000, 1000),
            photo("b", 1600, 1000),
            photo("c", 1000, 1600),
            photo("d", 1000, 1000),
            photo("e", 1600, 1000),
            photo("f", 1000, 1600),
        },
        1
    );

    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&photos);
    layout.setAvailableWidth(300);
    layout.setTargetRowHeight(96);
    require(layout.rowCount() >= 2, "the fixture must produce several visual rows");

    const QVariantMap right =
        layout.navigationTarget(QStringLiteral("a"), QStringLiteral("representation-a"), 1, 0);
    require(
        right.value(QStringLiteral("photoId")).toString() == QStringLiteral("b"),
        "Right must follow catalog order inside a visual row"
    );

    const QVariantMap down =
        layout.navigationTarget(QStringLiteral("b"), QStringLiteral("representation-b"), 0, 1);
    require(
        !down.isEmpty() && down.value(QStringLiteral("layoutRow")).toInt() == 1,
        "Down must enter the next visual row"
    );
}

void preserves_missing_source_roles_through_the_gallery_projection() {
    ReviewItem missing = photo("missing", 6000, 4000);
    missing.location_id = QStringLiteral("missing-location");
    missing.source_path = QStringLiteral("/missing/original.raw");
    missing.source_available = false;

    ReviewModel photos;
    photos.replace({missing}, 1);
    QSortFilterProxyModel filtered;
    filtered.setSourceModel(&photos);

    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&filtered);
    layout.setAvailableWidth(600);
    const QVariantList items =
        layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::ItemsRole).toList();
    require(items.size() == 1, "the missing photo must remain in the gallery projection");
    const QVariantMap projected = items.front().toMap();
    require(
        projected.value(QStringLiteral("locationId")).toString()
                == QStringLiteral("missing-location")
            && projected.contains(QStringLiteral("sourceAvailable"))
            && !projected.value(QStringLiteral("sourceAvailable")).toBool(),
        "the gallery projection must preserve the exact location and unavailable state"
    );
}

void ordered_sections_split_rows_without_hiding_unassigned_photos() {
    ReviewModel photos;
    photos.replace(
        {
            photo("a", 1200, 800),
            photo("b", 1200, 800),
            photo("c", 1200, 800),
            photo("d", 1200, 800),
        },
        1
    );

    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&photos);
    layout.setAvailableWidth(900);
    layout.setSections({
        QVariantMap{
            {QStringLiteral("key"), QStringLiteral("strong")},
            {QStringLiteral("title"), QStringLiteral("Highly related")},
            {QStringLiteral("subtitle"), QStringLiteral("Best matches")},
            {QStringLiteral("representationKeys"),
             QStringList{
                 QStringLiteral("a\u001frepresentation-a"),
                 QStringLiteral("b\u001frepresentation-b"),
             }},
        },
        QVariantMap{
            {QStringLiteral("key"), QStringLiteral("possible")},
            {QStringLiteral("title"), QStringLiteral("Possibly related")},
            {QStringLiteral("representationKeys"),
             QStringList{QStringLiteral("c\u001frepresentation-c")}},
        },
    });

    require(layout.rowCount() == 5, "two headings, two grouped rows, and one fallback row remain");
    require(
        layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::RowKindRole).toString()
                == QStringLiteral("section")
            && layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::SectionTitleRole)
                       .toString()
                   == QStringLiteral("Highly related")
            && layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::SectionItemCountRole)
                       .toInt()
                   == 2,
        "the first group heading exposes its title and exact visible count"
    );
    require(
        layout.data(layout.index(2, 0), JustifiedReviewLayoutModel::SectionKeyRole).toString()
            == QStringLiteral("possible"),
        "the second group starts only after the first group's photo rows"
    );
    const QVariantMap unassigned =
        layout.data(layout.index(4, 0), JustifiedReviewLayoutModel::ItemsRole)
            .toList()
            .front()
            .toMap();
    require(
        unassigned.value(QStringLiteral("photoId")).toString() == QStringLiteral("d"),
        "a partial grouping projection must not silently hide unassigned filtered photos"
    );

    const QVariantMap down =
        layout.navigationTarget(QStringLiteral("b"), QStringLiteral("representation-b"), 0, 1);
    require(
        down.value(QStringLiteral("photoId")).toString() == QStringLiteral("c")
            && down.value(QStringLiteral("layoutRow")).toInt() == 3,
        "keyboard navigation skips section headings and reaches the next photo row"
    );
}

void section_anchors_follow_visible_header_rows() {
    ReviewModel photos;
    photos.replace(
        {
            photo("a", 1200, 800),
            photo("b", 1200, 800),
            photo("c", 1200, 800),
        },
        1
    );

    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&photos);
    layout.setAvailableWidth(900);
    layout.setSections({
        QVariantMap{
            {QStringLiteral("key"), QStringLiteral("march")},
            {QStringLiteral("title"), QStringLiteral("March 2024 · Beijing")},
            {QStringLiteral("navigationLabel"), QStringLiteral("March 2024")},
            {QStringLiteral("navigationShortLabel"), QStringLiteral("Mar")},
            {QStringLiteral("navigationMajorLabel"), QStringLiteral("2024")},
            {QStringLiteral("representationKeys"),
             QStringList{
                 QStringLiteral("a\u001frepresentation-a"),
                 QStringLiteral("b\u001frepresentation-b"),
             }},
        },
        QVariantMap{
            {QStringLiteral("key"), QStringLiteral("april")},
            {QStringLiteral("title"), QStringLiteral("April 2024 · Shanghai")},
            {QStringLiteral("navigationLabel"), QStringLiteral("April 2024")},
            {QStringLiteral("navigationShortLabel"), QStringLiteral("Apr")},
            {QStringLiteral("navigationMajorLabel"), QStringLiteral("2024")},
            {QStringLiteral("representationKeys"),
             QStringList{QStringLiteral("c\u001frepresentation-c")}},
        },
    });

    const QVariantList anchors = layout.sectionAnchors();
    require(anchors.size() == 2, "every visible section header exposes one jump anchor");
    const QVariantMap first = anchors.at(0).toMap();
    const QVariantMap second = anchors.at(1).toMap();
    require(
        first.value(QStringLiteral("rowIndex")).toInt() == 0
            && second.value(QStringLiteral("rowIndex")).toInt() == 2,
        "anchors point at concrete header rows after each section's photo layout"
    );
    require(
        first.value(QStringLiteral("navigationShortLabel")).toString() == QStringLiteral("Mar")
            && first.value(QStringLiteral("navigationMajorLabel")).toString()
                   == QStringLiteral("2024")
            && first.value(QStringLiteral("title")).toString()
                   == QStringLiteral("March 2024 · Beijing"),
        "layout anchors preserve navigator labels without reconstructing grouping semantics"
    );
}

} // namespace

int main() {
    preserves_aspect_ratio_and_never_crops();
    density_changes_the_target_thumbnail_scale();
    keyboard_navigation_follows_rows_and_nearest_columns();
    preserves_missing_source_roles_through_the_gallery_projection();
    ordered_sections_split_rows_without_hiding_unassigned_photos();
    section_anchors_follow_visible_header_rows();
    return EXIT_SUCCESS;
}
