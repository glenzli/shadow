#include "review_filter_model.hpp"
#include "review_model.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "review filter model contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] ReviewItem item(
    const char* const photo_id,
    const char* const representation_id,
    const char* const flag,
    const int rating,
    const bool edited = false,
    const bool liked = false
) {
    ReviewItem value;
    value.photo_id = QString::fromLatin1(photo_id);
    value.representation_id = QString::fromLatin1(representation_id);
    value.decision_flag = QString::fromLatin1(flag);
    value.decision_rating = rating;
    value.has_development_edits = edited;
    value.liked = liked;
    return value;
}

void combined_lightroom_filters_intersect() {
    ReviewModel source;
    source.replace(
        {
            item("photo-a", "representation-a", "picked", 5, true, true),
            item("photo-b", "representation-b", "rejected", 3),
            item("photo-c", "representation-c", "unflagged", 1),
        },
        1
    );
    require(
        source.updateLibraryState(QStringLiteral("photo-a"), true, QStringLiteral("blue"), 1)
            && source
                   .updateLibraryState(QStringLiteral("photo-b"), false, QStringLiteral("red"), 2),
        "the source model must accept Catalog-authoritative color-label receipts"
    );

    ReviewFilterModel filtered;
    filtered.setSourceModel(&source);
    require(filtered.rowCount() == 3, "the default filter must include all photos");

    filtered.setFlagFilter(QStringLiteral("picked"));
    require(filtered.hasActiveServerFilter(), "a selected facet must require a server query");
    require(filtered.rowCount() == 1, "flag filtering must narrow the grid");
    filtered.setMinimumRating(4);
    require(filtered.rowCount() == 1, "star filtering must use a minimum rating threshold");
    filtered.setColorFilter(QStringLiteral("blue"));
    require(filtered.rowCount() == 1, "a matching color label must retain the selected photo");
    filtered.setColorFilter(QStringLiteral("red"));
    require(filtered.rowCount() == 0, "all enabled Lightroom-style filter facets must intersect");
    filtered.clearFilters();
    require(!filtered.hasActiveServerFilter(), "clearing facets must restore the all-photos query");
    filtered.setEditFilter(QStringLiteral("edited"));
    require(
        filtered.rowCount() == 1,
        "the edited facet must select photos with a working development Recipe"
    );
    filtered.setEditFilter(QStringLiteral("unedited"));
    require(
        filtered.rowCount() == 2,
        "the unedited facet must exclude photos with development edits"
    );
    filtered.clearFilters();
    filtered.setLikedFilter(QStringLiteral("liked"));
    require(filtered.rowCount() == 1, "the liked facet must select only Catalog-liked photos");
    filtered.setLikedFilter(QStringLiteral("unliked"));
    require(filtered.rowCount() == 2, "the unliked facet must exclude Catalog-liked photos");
    filtered.clearFilters();
    filtered.setExcludedFlagFilter(QStringLiteral("rejected"));
    filtered.setExcludedColorFilter(QStringLiteral("blue"));
    require(
        filtered.rowCount() == 1,
        "advanced exclusions must intersect with positive filter dimensions"
    );
    require(
        !filtered.hasActiveServerFilter(),
        "client exclusions must not be projected as unsupported catalog predicates"
    );
    filtered.setFlagFilter(QStringLiteral("unflagged"));
    require(
        filtered.rowCount() == 1,
        "a positive flag and negative color condition must compose with AND"
    );
    filtered.clearFilters();
    require(filtered.rowCount() == 3, "clearing filters must restore the grid");
}

void catalog_metadata_facets_remain_typed_server_filters() {
    ReviewFilterModel filtered;
    require(!filtered.hasActiveServerFilter(), "empty metadata facets must be absent");

    filtered.setCaptureMonth(QStringLiteral("2026-07"));
    filtered.setChineseLunarMonth(8);
    filtered.setChineseLunarDay(15);
    filtered.setChineseLunarMonthType(QStringLiteral("leap"));
    filtered.setCameraKey(QStringLiteral("Canon EOS R"));
    filtered.setLensKey(QStringLiteral("RF24-105MM F4 L IS USM"));
    filtered.setCountryKey(QStringLiteral(" CN "));
    filtered.setLocalityKey(QStringLiteral("CN\u001fShanghai\u001fShanghai"));
    filtered.setTravelFilterEnabled(true);
    require(
        filtered.captureMonth() == QStringLiteral("2026-07"),
        "capture months must retain the canonical YYYY-MM key"
    );
    require(
        filtered.chineseLunarMonth() == 8 && filtered.chineseLunarDay() == 15,
        "Chinese lunar month and day must remain typed recurring predicates"
    );
    require(
        filtered.chineseLunarMonthType() == QStringLiteral("leap"),
        "Chinese lunar leap-month identity must remain explicit"
    );
    require(
        filtered.cameraKey() == QStringLiteral("canon eos r"),
        "camera facet keys must use catalog-compatible normalization"
    );
    require(
        filtered.lensKey() == QStringLiteral("rf24-105mm f4 l is usm"),
        "lens facet keys must use catalog-compatible normalization"
    );
    require(
        filtered.countryKey() == QStringLiteral("cn"),
        "country facet keys must use catalog-compatible normalization"
    );
    require(
        filtered.localityKey() == QStringLiteral("cn\u001fshanghai\u001fshanghai"),
        "city facet keys must remain structured Catalog identities"
    );
    require(filtered.travelFilterEnabled(), "Travel is an explicit generated server filter");
    require(filtered.hasActiveServerFilter(), "metadata facets must request a fresh page");

    filtered.setDailyFilterEnabled(true);
    require(
        filtered.dailyFilterEnabled() && !filtered.travelFilterEnabled(),
        "Daily must replace the complementary Travel predicate"
    );

    filtered.setCaptureMonth(QStringLiteral("2026-13"));
    filtered.setChineseLunarMonth(99);
    filtered.setChineseLunarDay(-1);
    filtered.setChineseLunarMonthType(QStringLiteral("regional-calendar"));
    require(
        filtered.captureMonth().isEmpty(),
        "invalid capture months must fail closed instead of widening a query"
    );
    require(
        filtered.chineseLunarMonth() == 0 && filtered.chineseLunarDay() == 0
            && filtered.chineseLunarMonthType() == QStringLiteral("all"),
        "lunar predicates must reject numeric bounds and unknown month types"
    );
    filtered.clearFilters();
    require(
        !filtered.hasActiveServerFilter() && !filtered.travelFilterEnabled()
            && !filtered.dailyFilterEnabled() && filtered.chineseLunarMonth() == 0
            && filtered.chineseLunarDay() == 0
            && filtered.chineseLunarMonthType() == QStringLiteral("all"),
        "clearing must include metadata, Daily, and Travel facets"
    );
}

void hierarchical_keyword_filters_are_normalized_server_predicates() {
    ReviewFilterModel filtered;
    filtered.setKeywordIdsAll(
        {QStringLiteral(" keyword-a "),
         QStringLiteral("keyword-b"),
         QStringLiteral("keyword-a"),
         QString()}
    );
    filtered.setExcludedKeywordIdsAny({QStringLiteral("keyword-c"), QStringLiteral(" keyword-c ")});

    require(
        filtered.keywordIdsAll()
            == QStringList{
                QStringLiteral("keyword-a"),
                QStringLiteral("keyword-b"),
            },
        "required keyword ids must trim, preserve order, and deduplicate"
    );
    require(
        filtered.excludedKeywordIdsAny() == QStringList{QStringLiteral("keyword-c")},
        "excluded keyword ids must trim and deduplicate"
    );
    require(
        filtered.hasActiveServerFilter(),
        "keyword predicates must always request a Catalog query"
    );

    filtered.clearFilters();
    require(
        filtered.keywordIdsAll().isEmpty() && filtered.excludedKeywordIdsAny().isEmpty(),
        "clearing filters must remove both keyword predicate sets"
    );
}

void remote_rows_participate_only_in_locally_evaluable_filters() {
    ReviewItem local = item("local-photo", "local-representation", "picked", 5, false, true);
    ReviewItem remote =
        item("remote:server:photo", "remote:server:representation", "picked", 5, false, true);
    remote.is_remote = true;
    remote.camera_make = QStringLiteral(" NIKON ");
    remote.camera_model = QStringLiteral("Z9");
    remote.capture_day = QStringLiteral("2026-09-06");

    ReviewModel source;
    source.replace({local, remote}, 1);
    ReviewFilterModel filtered;
    filtered.setSourceModel(&source);

    filtered.setFlagFilter(QStringLiteral("picked"));
    filtered.setLikedFilter(QStringLiteral("liked"));
    require(
        filtered.rowCount() == 2,
        "remote rows must participate in locally mirrored flag, rating, like, and color filters"
    );

    filtered.setCaptureMonth(QStringLiteral("2026-09"));
    filtered.setCameraKey(QStringLiteral("nikon\u001fz9"));
    require(
        filtered.rowCount() == 2,
        "known remote date and equipment metadata participates in matching facets"
    );
    filtered.setCameraKey(QStringLiteral("sony\u001filce-7"));
    require(filtered.rowCount() == 1, "nonmatching remote equipment is excluded");
    filtered.setCameraKey({});
    filtered.setCaptureMonth({});
    filtered.setCountryKey(QStringLiteral("cn"));
    require(
        filtered.rowCount() == 1,
        "remote rows without a server-side place contract must not leak into local place facets"
    );
    filtered.setCountryKey({});
    filtered.setKeywordIdsAll({QStringLiteral("keyword-a")});
    require(
        filtered.rowCount() == 1,
        "remote rows without synchronized keyword membership must not enter keyword results"
    );
}

void semantic_results_compose_as_a_ranked_client_filter() {
    ReviewModel source;
    source.replace(
        {
            item("photo-a", "representation-a", "picked", 5),
            item("photo-b", "representation-b", "picked", 4),
            item("photo-c", "representation-c", "rejected", 3),
        },
        1
    );
    ReviewFilterModel filtered;
    filtered.setSourceModel(&source);
    filtered.setSemanticRepresentationOrder({
        QStringLiteral("photo-b\u001frepresentation-b"),
        QStringLiteral("photo-a\u001frepresentation-a"),
    });
    require(filtered.semanticFilterActive(), "semantic ranking must activate a client filter");
    require(
        !filtered.hasActiveServerFilter(),
        "semantic ranking must not be projected as an unsupported Catalog predicate"
    );
    require(filtered.rowCount() == 2, "only ranked exact representations remain visible");
    require(
        filtered.data(filtered.index(0, 0), ReviewModel::PhotoIdRole).toString()
            == QStringLiteral("photo-b"),
        "the grid must follow semantic similarity order"
    );
    filtered.setMinimumRating(5);
    require(
        filtered.rowCount() == 1
            && filtered.data(filtered.index(0, 0), ReviewModel::PhotoIdRole).toString()
                   == QStringLiteral("photo-a"),
        "semantic ranking must intersect with ordinary Library filters"
    );
    filtered.clearFilters();
    require(
        !filtered.semanticFilterActive() && filtered.rowCount() == 3,
        "clearing all filters must restore source order and every row"
    );
}

void smart_categories_filter_exact_members_without_reordering() {
    ReviewModel source;
    source.replace(
        {
            item("photo-a", "representation-a", "picked", 5),
            item("photo-b", "representation-b", "picked", 4),
            item("photo-c", "representation-c", "picked", 3),
        },
        1
    );
    ReviewFilterModel filtered;
    filtered.setSourceModel(&source);
    filtered.setSmartCategoryRepresentationKeys({
        QStringLiteral("photo-c\u001frepresentation-c"),
        QStringLiteral("photo-a\u001frepresentation-a"),
    });
    require(filtered.smartCategoryFilterActive(), "smart category selection activates filtering");
    require(filtered.rowCount() == 2, "only exact category members remain visible");
    require(
        filtered.data(filtered.index(0, 0), ReviewModel::PhotoIdRole).toString()
            == QStringLiteral("photo-a"),
        "smart categories preserve the current gallery order"
    );
    filtered.clearFilters();
    require(
        !filtered.smartCategoryFilterActive() && filtered.rowCount() == 3,
        "clearing filters removes smart-category membership"
    );
}

void offline_availability_filters_preserve_cached_previews_and_unknown_origins() {
    ReviewModel source;
    auto local = item("local", "local-rep", "unflagged", 0);
    auto unavailable = item("offline", "offline-rep", "unflagged", 0);
    unavailable.is_remote = true;
    unavailable.remote_offline = true;
    unavailable.source_available = false;
    auto preview = unavailable;
    preview.photo_id = "preview";
    preview.representation_id = "preview-rep";
    preview.has_visual = true;
    preview.visual_source_override = "file:///cached-preview.jpg";
    auto original = unavailable;
    original.photo_id = "original";
    original.representation_id = "original-rep";
    original.remote_original_cached = true;
    original.source_available = true;
    auto unknown = unavailable;
    unknown.photo_id = "unknown";
    unknown.representation_id = "unknown-rep";
    unknown.remote_offline = false;
    source.replace({local, unavailable, preview, original, unknown}, 1);
    ReviewFilterModel filter;
    filter.setSourceModel(&source);
    filter.setHideOfflineUncached(true);
    require(filter.rowCount() == 4, "only confirmed offline uncached rows are hidden");
    require(!filter.hasActiveServerFilter(), "availability never changes the catalog query");
    filter.setOnlyEditable(true);
    require(filter.rowCount() == 2, "preview-only and unknown original are not editable");
    filter.clearFilters();
    require(
        filter.rowCount() == 5 && !filter.onlyEditable() && !filter.hideOfflineUncached(),
        "clear restores all availability states"
    );
}

} // namespace

int main() {
    offline_availability_filters_preserve_cached_previews_and_unknown_origins();
    combined_lightroom_filters_intersect();
    catalog_metadata_facets_remain_typed_server_filters();
    hierarchical_keyword_filters_are_normalized_server_predicates();
    remote_rows_participate_only_in_locally_evaluable_filters();
    semantic_results_compose_as_a_ranked_client_filter();
    smart_categories_filter_exact_members_without_reordering();
    return EXIT_SUCCESS;
}
