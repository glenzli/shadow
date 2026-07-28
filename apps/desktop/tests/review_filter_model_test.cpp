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
        source.updateLibraryState(
            QStringLiteral("photo-a"),
            true,
            QStringLiteral("blue"),
            1
        )
            && source.updateLibraryState(
                QStringLiteral("photo-b"),
                false,
                QStringLiteral("red"),
                2
            ),
        "the source model must accept Catalog-authoritative color-label receipts"
    );

    ReviewFilterModel filtered;
    filtered.setSourceModel(&source);
    require(filtered.rowCount() == 3, "the default filter must include all photos");

    filtered.setFlagFilter(QStringLiteral("picked"));
    require(filtered.hasActiveServerFilter(), "a selected facet must require a server query");
    require(filtered.rowCount() == 1, "flag filtering must narrow the grid");
    filtered.setMinimumRating(4);
    require(
        filtered.rowCount() == 1,
        "star filtering must use a minimum rating threshold"
    );
    filtered.setColorFilter(QStringLiteral("blue"));
    require(
        filtered.rowCount() == 1,
        "a matching color label must retain the selected photo"
    );
    filtered.setColorFilter(QStringLiteral("red"));
    require(
        filtered.rowCount() == 0,
        "all enabled Lightroom-style filter facets must intersect"
    );
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
    require(
        filtered.rowCount() == 1,
        "the liked facet must select only Catalog-liked photos"
    );
    filtered.setLikedFilter(QStringLiteral("unliked"));
    require(
        filtered.rowCount() == 2,
        "the unliked facet must exclude Catalog-liked photos"
    );
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
    filtered.setCameraKey(QStringLiteral("Canon EOS R"));
    filtered.setLensKey(QStringLiteral("RF24-105MM F4 L IS USM"));
    require(
        filtered.captureMonth() == QStringLiteral("2026-07"),
        "capture months must retain the canonical YYYY-MM key"
    );
    require(
        filtered.cameraKey() == QStringLiteral("canon eos r"),
        "camera facet keys must use catalog-compatible normalization"
    );
    require(
        filtered.lensKey() == QStringLiteral("rf24-105mm f4 l is usm"),
        "lens facet keys must use catalog-compatible normalization"
    );
    require(filtered.hasActiveServerFilter(), "metadata facets must request a fresh page");

    filtered.setCaptureMonth(QStringLiteral("2026-13"));
    require(
        filtered.captureMonth().isEmpty(),
        "invalid capture months must fail closed instead of widening a query"
    );
    filtered.clearFilters();
    require(!filtered.hasActiveServerFilter(), "clearing must include metadata facets");
}

} // namespace

int main() {
    combined_lightroom_filters_intersect();
    catalog_metadata_facets_remain_typed_server_filters();
    return EXIT_SUCCESS;
}
