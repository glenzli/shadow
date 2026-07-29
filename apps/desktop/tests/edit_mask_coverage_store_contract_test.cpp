#include "edit_preview_provider.hpp"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Edit mask coverage store contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] EditMaskCoveragePayload
coverage_payload(const std::uint32_t target, const std::uint64_t selection) {
    QByteArray samples;
    samples.append(char{0});
    samples.append(char{64});
    samples.append(static_cast<char>(128));
    samples.append(static_cast<char>(255));
    return {
        .samples = std::move(samples),
        .dimensions = QSize(2, 2),
        .row_stride_bytes = 2,
        .version = EDIT_MASK_COVERAGE_VERSION,
        .target_layer_index = target,
        .selection_revision = selection,
    };
}

[[nodiscard]] QString provider_request(const MaskCoverageGeneration generation) {
    return QStringLiteral(
               "scope/mask/current?photo=%1&recipe=%2&target=%3"
               "&selection=%4&preview=%5"
    )
        .arg(generation.photo)
        .arg(generation.recipe_revision)
        .arg(generation.target_layer_index)
        .arg(generation.selection_revision)
        .arg(generation.paired_preview_generation);
}

} // namespace

int main() {
    if (!require(
            invalidates_mask_coverage_for_edit(QStringLiteral("local_mask/brush"))
                && invalidates_mask_coverage_for_edit(QStringLiteral("local_mask/brush/clear"))
                && invalidates_mask_coverage_for_edit(QStringLiteral("local_mask/invert"))
                && !invalidates_mask_coverage_for_edit(QStringLiteral("basic/exposure")),
            "brush and other selected-mask mutations hide old coverage without "
            "invalidating it for ordinary Grade Node edits"
        )) {
        return EXIT_FAILURE;
    }

    auto store = std::make_shared<EditPreviewStore>();
    EditPreviewProvider provider(store);

    const MaskCoverageGeneration generation{
        .photo = 7,
        .recipe_revision = 11,
        .target_layer_index = 2,
        .selection_revision = 5,
        .paired_preview_generation = 17,
    };
    store->expectMaskCoverage(generation);
    store->publish(
        EditPreviewSlot::Current,
        QByteArray(12, char{42}),
        QSize(2, 2),
        6,
        {},
        generation.paired_preview_generation
    );
    if (!require(
            store->publishMaskCoverage(
                coverage_payload(generation.target_layer_index, generation.selection_revision),
                generation
            ),
            "an exact expected generation publishes after its paired preview"
        )) {
        return EXIT_FAILURE;
    }

    QSize image_size;
    const QImage image = provider.requestImage(provider_request(generation), &image_size, {});
    if (!require(
            !image.isNull() && image.format() == QImage::Format_Alpha8 && image_size == QSize(2, 2),
            "the provider exposes the renderer's R8 coverage as an Alpha8 image"
        )
        || !require(
            image.constScanLine(0)[0] == 0U && image.constScanLine(0)[1] == 64U
                && image.constScanLine(1)[0] == 128U && image.constScanLine(1)[1] == 255U,
            "the provider preserves every coverage sample without color conversion"
        )) {
        return EXIT_FAILURE;
    }

    MaskCoverageGeneration stale_selection = generation;
    ++stale_selection.selection_revision;
    if (!require(
            provider.requestImage(provider_request(stale_selection), nullptr, {}).isNull(),
            "a mismatched selection revision cannot read stored coverage"
        )) {
        return EXIT_FAILURE;
    }

    MaskCoverageGeneration next_generation = generation;
    ++next_generation.recipe_revision;
    ++next_generation.paired_preview_generation;
    store->expectMaskCoverage(next_generation);
    if (!require(
            !store->publishMaskCoverage(
                coverage_payload(generation.target_layer_index, generation.selection_revision),
                generation
            ),
            "an older in-flight generation cannot publish after expectation advances"
        )
        || !require(
            !store->publishMaskCoverage(
                coverage_payload(
                    next_generation.target_layer_index,
                    next_generation.selection_revision
                ),
                next_generation
            ),
            "coverage cannot publish before its exact preview generation"
        )) {
        return EXIT_FAILURE;
    }

    store->publish(
        EditPreviewSlot::Current,
        QByteArray(12, char{43}),
        QSize(2, 2),
        6,
        {},
        next_generation.paired_preview_generation
    );
    EditMaskCoveragePayload malformed =
        coverage_payload(next_generation.target_layer_index, next_generation.selection_revision);
    malformed.row_stride_bytes = 3;
    if (!require(
            !store->publishMaskCoverage(std::move(malformed), next_generation),
            "a non-tightly-packed R8 payload fails closed"
        )
        || !require(
            store->publishMaskCoverage(
                coverage_payload(
                    next_generation.target_layer_index,
                    next_generation.selection_revision
                ),
                next_generation
            ),
            "the valid replacement remains publishable after malformed input"
        )) {
        return EXIT_FAILURE;
    }

    store->clearMaskCoverage();
    if (!require(
            store->maskCoverageSnapshot(next_generation).samples.isEmpty(),
            "clearing mask state removes the transient coverage"
        )
        || !require(
            !store->snapshot(EditPreviewSlot::Current, next_generation.paired_preview_generation)
                 .bytes.isEmpty(),
            "clearing mask state never clears the paired current preview"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
