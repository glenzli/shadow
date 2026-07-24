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
    const bool edited = false
) {
    ReviewItem value;
    value.photo_id = QString::fromLatin1(photo_id);
    value.representation_id = QString::fromLatin1(representation_id);
    value.decision_flag = QString::fromLatin1(flag);
    value.decision_rating = rating;
    value.has_development_edits = edited;
    return value;
}

void combined_lightroom_filters_intersect() {
    ReviewModel source;
    source.replace(
        {
            item("photo-a", "representation-a", "picked", 5, true),
            item("photo-b", "representation-b", "rejected", 3),
            item("photo-c", "representation-c", "unflagged", 1),
        },
        1
    );
    require(
        source.setColorLabel(QStringLiteral("photo-a"), QStringLiteral("blue"))
            && source.setColorLabel(
                QStringLiteral("photo-b"),
                QStringLiteral("red")
            ),
        "the source model must accept valid local color labels"
    );

    ReviewFilterModel filtered;
    filtered.setSourceModel(&source);
    require(filtered.rowCount() == 3, "the default filter must include all photos");

    filtered.setFlagFilter(QStringLiteral("picked"));
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
    require(filtered.rowCount() == 3, "clearing filters must restore the grid");
}

} // namespace

int main() {
    combined_lightroom_filters_intersect();
    return EXIT_SUCCESS;
}
