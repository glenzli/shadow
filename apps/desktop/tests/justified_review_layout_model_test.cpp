#include "justified_review_layout_model.hpp"
#include "review_model.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
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

class CountedProjection final : public QSortFilterProxyModel {
  public:
    mutable int reads = 0;
    mutable int role_names_reads = 0;
    mutable int title_reads = 0;
    QVariant data(const QModelIndex& index, int role) const override {
        ++reads;
        if (role == ReviewModel::TitleRole) {
            ++title_reads;
        }
        return QSortFilterProxyModel::data(index, role);
    }
    QHash<int, QByteArray> roleNames() const override {
        ++role_names_reads;
        return QSortFilterProxyModel::roleNames();
    }
    void notify(const QList<int>& roles) {
        emit dataChanged(index(0, 0), index(0, 0), roles);
    }
};

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

void coalesces_tail_insertions_without_resetting_visible_rows() {
    ReviewModel photos;
    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&photos);
    layout.setAvailableWidth(900);

    int layout_resets = 0;
    int inserted_rows = 0;
    QObject::connect(&layout, &QAbstractItemModel::modelReset, [&layout_resets]() {
        ++layout_resets;
    });
    QObject::connect(
        &layout,
        &QAbstractItemModel::rowsInserted,
        [&inserted_rows](const QModelIndex&, int first, int last) {
            inserted_rows += last - first + 1;
        }
    );

    photos.append({photo("a", 1200, 800)});
    photos.append({photo("b", 1200, 800)});
    photos.append({photo("c", 1200, 800)});
    require(
        layout_resets == 0 && inserted_rows == 0,
        "source changes must defer the gallery update"
    );

    QCoreApplication::processEvents();
    require(
        layout_resets == 0 && inserted_rows == 1 && layout.rowCount() == 1,
        "one event-loop turn must combine tail insertions without resetting the gallery"
    );
}

void appending_after_an_unfinished_row_reflows_only_the_tail() {
    ReviewModel photos;
    photos.replace({photo("a", 1200, 800), photo("b", 1200, 800), photo("c", 1200, 800)}, 1);
    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&photos);
    layout.setAvailableWidth(900);
    require(layout.rowCount() == 1, "three photos initially share the unfinished last row");
    int resets = 0;
    int changes = 0;
    int insertions = 0;
    QObject::connect(&layout, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    QObject::connect(
        &layout,
        &QAbstractItemModel::dataChanged,
        [&changes](const QModelIndex&, const QModelIndex&, const QList<int>&) { ++changes; }
    );
    QObject::connect(
        &layout,
        &QAbstractItemModel::rowsInserted,
        [&insertions](const QModelIndex&, int first, int last) { insertions += last - first + 1; }
    );
    require(
        photos.appendSnapshot({photo("d", 1200, 800), photo("e", 1200, 800)}, 1),
        "the next page must append"
    );
    QCoreApplication::processEvents();
    require(
        resets == 0 && changes == 1 && insertions == 1 && layout.rowCount() == 2,
        "the prior unfinished row must update while the new tail is inserted"
    );
    require(
        layout.navigationTarget("d", "representation-d", 0, 0).value("layoutRow").toInt() == 0
            && layout.navigationTarget("e", "representation-e", 0, 0).value("layoutRow").toInt()
                   == 1,
        "both pages must retain the intended photo order after reflow"
    );
}

void pending_source_reorder_never_presents_another_photos_metadata() {
    ReviewModel photos;
    const ReviewItem first = photo("a", 1200, 800);
    const ReviewItem second = photo("b", 1200, 800);
    photos.replace({first, second}, 1);
    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&photos);
    layout.setAvailableWidth(900);
    photos.replace({second, first}, 2);
    const QVariantList before_rebuild =
        layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::ItemsRole).toList();
    require(
        before_rebuild.at(0).toMap().value(QStringLiteral("photoId")) == QStringLiteral("a")
            && before_rebuild.at(0).toMap().value(QStringLiteral("title")) == QStringLiteral("a")
            && before_rebuild.at(1).toMap().value(QStringLiteral("photoId")) == QStringLiteral("b")
            && before_rebuild.at(1).toMap().value(QStringLiteral("title")) == QStringLiteral("b"),
        "a pending source reset must not pair old geometry with another photo's metadata"
    );
    QCoreApplication::processEvents();
    const QVariantList after_rebuild =
        layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::ItemsRole).toList();
    require(
        after_rebuild.at(0).toMap().value(QStringLiteral("photoId")) == QStringLiteral("b")
            && after_rebuild.at(0).toMap().value(QStringLiteral("title")) == QStringLiteral("b"),
        "the rebuilt row must follow the new source order"
    );
}

void large_library_page_append_reads_only_the_new_tail() {
    ReviewModel photos;
    QVector<ReviewItem> initial;
    initial.reserve(10'000);
    for (int i = 0; i < 10'000; ++i) {
        auto item = photo("fixture", 1200, 800);
        item.photo_id = QString::number(i);
        item.representation_id = QStringLiteral("representation-") + item.photo_id;
        initial.push_back(std::move(item));
    }
    photos.replace(std::move(initial), 1);
    CountedProjection source;
    source.setSourceModel(&photos);
    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&source);
    layout.setAvailableWidth(900);
    require(
        source.role_names_reads <= 2,
        "large-gallery construction must reuse the source role table across photos"
    );
    require(source.title_reads == 0, "layout construction must not retain every card's metadata");
    const QVariantList first_visible_row =
        layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::ItemsRole).toList();
    require(
        !first_visible_row.isEmpty()
            && first_visible_row.front().toMap().value(QStringLiteral("title"))
                   == QStringLiteral("fixture")
            && source.title_reads == first_visible_row.size(),
        "a visible row must still receive full source metadata on demand"
    );
    const QVariantMap old_position = layout.navigationTarget("42", "representation-42", 0, 0);
    int resets = 0;
    int changed_rows = 0;
    int inserted_rows = 0;
    QObject::connect(&layout, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    QObject::connect(
        &layout,
        &QAbstractItemModel::dataChanged,
        [&changed_rows](const QModelIndex&, const QModelIndex&, const QList<int>&) {
            ++changed_rows;
        }
    );
    QObject::connect(
        &layout,
        &QAbstractItemModel::rowsInserted,
        [&inserted_rows](const QModelIndex&, int first, int last) {
            inserted_rows += last - first + 1;
        }
    );

    QVector<ReviewItem> page;
    for (int i = 10'000; i < 10'096; ++i) {
        auto item = photo("fixture", 1200, 800);
        item.photo_id = QString::number(i);
        item.representation_id = QStringLiteral("representation-") + item.photo_id;
        page.push_back(std::move(item));
    }
    source.reads = 0;
    source.role_names_reads = 0;
    source.title_reads = 0;
    require(photos.appendSnapshot(std::move(page), 1), "the new page must be accepted");
    QCoreApplication::processEvents();
    require(source.title_reads == 0, "appending offscreen photos must not fetch card metadata");
    require(
        source.role_names_reads <= 2,
        "page append must reuse one source role table for the whole page"
    );
    const QVariantMap retained = layout.navigationTarget("42", "representation-42", 0, 0);
    const QVariantMap appended = layout.navigationTarget("10095", "representation-10095", 0, 0);
    require(
        resets == 0 && inserted_rows > 0 && changed_rows <= 1,
        "paging must update only the previous tail and newly inserted rows"
    );
    require(
        source.reads < 20'000,
        "a 96-photo page must not read the 10000 previously projected photos"
    );
    require(
        retained.value("layoutRow") == old_position.value("layoutRow")
            && retained.value("layoutX") == old_position.value("layoutX")
            && appended.value("photoId") == QStringLiteral("10095"),
        "existing scroll geometry and appended-photo navigation remain valid"
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
    require(
        layout.data(layout.index(1, 0), JustifiedReviewLayoutModel::SectionKeyRole).toString()
                == QStringLiteral("strong")
            && layout.data(layout.index(3, 0), JustifiedReviewLayoutModel::SectionKeyRole)
                       .toString()
                   == QStringLiteral("possible")
            && layout.data(layout.index(4, 0), JustifiedReviewLayoutModel::SectionKeyRole)
                   .toString()
                   .isEmpty(),
        "photo rows retain their presentation section without assigning unrelated photos"
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

void grouped_photo_rows_keep_one_visible_stack_representative() {
    ReviewModel photos;
    photos.replace({photo("a", 1200, 800), photo("b", 1200, 800), photo("c", 1200, 800)}, 1);
    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&photos);
    layout.setTargetRowHeight(360);
    layout.setAvailableWidth(400);
    layout.setSections({QVariantMap{
        {QStringLiteral("key"), QStringLiteral("similar-review")},
        {QStringLiteral("title"), QStringLiteral("Similar review candidates")},
        {QStringLiteral("representationKeys"),
         QStringList{
             QStringLiteral("a\u001frepresentation-a"),
             QStringLiteral("b\u001frepresentation-b"),
             QStringLiteral("c\u001frepresentation-c"),
         }},
    }});
    require(layout.rowCount() == 4, "three candidates occupy separate photo rows");
    const QVariantMap anchor =
        layout.navigationTarget(QStringLiteral("a"), QStringLiteral("representation-a"), 0, 0);
    const QVariantMap next =
        layout.navigationTarget(QStringLiteral("b"), QStringLiteral("representation-b"), 0, 0);
    require(
        anchor.value(QStringLiteral("sectionLeadRow")).toBool()
            && !next.value(QStringLiteral("sectionLeadRow")).toBool()
            && next.value(QStringLiteral("sectionKey")) == QStringLiteral("similar-review"),
        "only the first photo row remains visible when a similar stack is collapsed"
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

void decisions_update_one_row_without_relayout_or_catalog_scan() {
    ReviewModel photos;
    QVector<ReviewItem> items;
    for (int i = 0; i < 10'000; ++i) {
        auto item = photo("fixture", 1200, 800);
        item.photo_id = QString::number(i);
        item.representation_id = QStringLiteral("representation-") + item.photo_id;
        items.append(item);
    }
    photos.replace(items, 1);
    CountedProjection source;
    source.setSourceModel(&photos);
    JustifiedReviewLayoutModel layout;
    layout.setSourceModel(&source);
    layout.setAvailableWidth(900);
    const auto before = layout.navigationTarget("9876", "representation-9876", 0, 0);
    int resets = 0;
    int changed = 0;
    QObject::connect(&layout, &QAbstractItemModel::modelReset, [&]() { ++resets; });
    QObject::connect(
        &layout,
        &QAbstractItemModel::dataChanged,
        [&](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
            require(
                first == last && roles == QList<int>{JustifiedReviewLayoutModel::ItemsRole},
                "badge mutation publishes only its containing row's items"
            );
            ++changed;
        }
    );
    source.reads = 0;
    QElapsedTimer timer;
    timer.start();
    require(photos.updateDecision("9876", 1, "picked", 4), "fixture decision updated");
    QCoreApplication::processEvents();
    const auto elapsed = timer.nsecsElapsed();
    const auto after = layout.navigationTarget("9876", "representation-9876", 0, 0);
    require(resets == 0 && changed == 1, "one rating must never reset the full gallery");
    require(source.reads < 300, "one decision may load its target card but not unrelated photos");
    require(
        after.value("decisionRating").toInt() == 4 && after.value("decisionFlag") == "picked",
        "updated rating and flag reach the gallery and keyboard snapshot"
    );
    require(
        before.value("layoutRow") == after.value("layoutRow")
            && before.value("layoutWidth") == after.value("layoutWidth")
            && before.value("layoutX") == after.value("layoutX"),
        "decision changes preserve placement and scroll geometry"
    );
    std::cout << "10000-photo decision: " << source.reads << " source reads, " << changed
              << " row update, " << resets << " resets, " << elapsed / 1000 << " us\n";
    source.notify({ReviewModel::VisualWidthRole});
    QCoreApplication::processEvents();
    require(resets == 1, "dimensions still rebuild geometry");
    source.notify({});
    QCoreApplication::processEvents();
    require(resets == 2, "unspecified source roles rebuild safely");

    // A filter removes the rated item before the deferred layout publication.
    source.setFilterRole(ReviewModel::DecisionRatingRole);
    source.setFilterFixedString("0");
    require(photos.updateDecision("12", 2, "picked", 5), "filtered decision updated");
    QCoreApplication::processEvents();
    const auto next = layout.navigationTarget("11", "representation-11", 1, 0);
    require(
        next.value("photoId") == "13",
        "filtered-out decisions leave no stale navigation entry"
    );
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    preserves_aspect_ratio_and_never_crops();
    density_changes_the_target_thumbnail_scale();
    keyboard_navigation_follows_rows_and_nearest_columns();
    preserves_missing_source_roles_through_the_gallery_projection();
    coalesces_tail_insertions_without_resetting_visible_rows();
    appending_after_an_unfinished_row_reflows_only_the_tail();
    pending_source_reorder_never_presents_another_photos_metadata();
    large_library_page_append_reads_only_the_new_tail();
    ordered_sections_split_rows_without_hiding_unassigned_photos();
    grouped_photo_rows_keep_one_visible_stack_representative();
    section_anchors_follow_visible_header_rows();
    decisions_update_one_row_without_relayout_or_catalog_scan();
    return EXIT_SUCCESS;
}
