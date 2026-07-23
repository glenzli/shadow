#include "justified_review_layout_model.hpp"
#include "review_model.hpp"

#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "justified review layout contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] ReviewItem photo(
    const char* const id,
    const std::uint32_t width,
    const std::uint32_t height
) {
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
    const QVariantList first_row = layout.data(
        layout.index(0, 0), JustifiedReviewLayoutModel::ItemsRole
    ).toList();
    require(!first_row.isEmpty(), "a row must expose its photo maps");

    for (const QVariant& variant : first_row) {
        const QVariantMap item = variant.toMap();
        const qreal expected = item.value(QStringLiteral("visualWidth")).toReal()
            / item.value(QStringLiteral("visualHeight")).toReal();
        const qreal actual = item.value(QStringLiteral("layoutWidth")).toReal()
            / layout.data(layout.index(0, 0), JustifiedReviewLayoutModel::RowHeightRole)
                .toReal();
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
    const int small_height = layout.data(
        layout.index(0, 0), JustifiedReviewLayoutModel::RowHeightRole
    ).toInt();
    layout.setTargetRowHeight(280);
    const int large_height = layout.data(
        layout.index(0, 0), JustifiedReviewLayoutModel::RowHeightRole
    ).toInt();
    require(
        large_height > small_height,
        "the toolbar density control must enlarge an incomplete final row"
    );
}

} // namespace

int main() {
    preserves_aspect_ratio_and_never_crops();
    density_changes_the_target_thumbnail_scale();
    return EXIT_SUCCESS;
}
