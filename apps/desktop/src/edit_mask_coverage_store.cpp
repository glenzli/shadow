#include "edit_preview_provider.hpp"

#include <QReadLocker>
#include <QWriteLocker>

#include <limits>
#include <utility>

namespace {

[[nodiscard]] bool valid_mask_coverage_descriptor(const EditMaskCoveragePayload& payload) noexcept {
    if (payload.version != EDIT_MASK_COVERAGE_VERSION || !payload.dimensions.isValid()
        || payload.dimensions.isEmpty()
        || payload.dimensions.width() > std::numeric_limits<int>::max()
        || payload.dimensions.height() > std::numeric_limits<int>::max()) {
        return false;
    }
    const std::uint64_t width = static_cast<std::uint64_t>(payload.dimensions.width());
    const std::uint64_t height = static_cast<std::uint64_t>(payload.dimensions.height());
    return payload.row_stride_bytes == width
           && width * height <= static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max());
}

[[nodiscard]] bool
valid_materialized_mask_coverage(const EditMaskCoveragePayload& payload) noexcept {
    if (!valid_mask_coverage_descriptor(payload)) {
        return false;
    }
    const auto expected_bytes = static_cast<std::uint64_t>(payload.dimensions.width())
                                * static_cast<std::uint64_t>(payload.dimensions.height());
    return static_cast<std::uint64_t>(payload.samples.size()) == expected_bytes;
}

} // namespace

void EditPreviewStore::expectMaskCoverage(const MaskCoverageGeneration generation) {
    QWriteLocker lock(&lock_);
    expected_mask_coverage_ = generation;
}

void EditPreviewStore::clearMaskCoverage() {
    QWriteLocker lock(&lock_);
    mask_coverage_ = {};
    expected_mask_coverage_.reset();
}

bool EditPreviewStore::publishMaskCoverage(
    EditMaskCoveragePayload payload,
    const MaskCoverageGeneration generation
) {
    if (!payload.samples.isEmpty() && !valid_materialized_mask_coverage(payload)) {
        return false;
    }

    QWriteLocker lock(&lock_);
    if (!expected_mask_coverage_.has_value()
        || !accepts_mask_coverage(generation, *expected_mask_coverage_)
        || generation.paired_preview_generation != current_.generation
        || (current_.frame == nullptr && current_.bytes.isEmpty())
        || payload.target_layer_index != generation.target_layer_index
        || payload.target_component_index != generation.target_component_index
        || payload.selection_revision != generation.selection_revision
        || payload.dimensions != current_.dimensions || !valid_mask_coverage_descriptor(payload)) {
        return false;
    }

    std::shared_ptr<const BackendEditPreviewFrame> frame;
    qsizetype row_stride_bytes = static_cast<qsizetype>(payload.row_stride_bytes);
    if (current_.frame != nullptr) {
        const auto coverage = current_.frame->maskCoverage();
        if (!payload.samples.isEmpty() || !coverage.has_value()
            || coverage->version != payload.version
            || coverage->target_layer_index != payload.target_layer_index
            || coverage->target_component_index != payload.target_component_index
            || coverage->selection_revision != payload.selection_revision
            || coverage->dimensions != payload.dimensions
            || coverage->row_stride_bytes != payload.row_stride_bytes
            || static_cast<std::uint64_t>(coverage->samples.size())
                   != static_cast<std::uint64_t>(payload.dimensions.width())
                          * static_cast<std::uint64_t>(payload.dimensions.height())) {
            return false;
        }
        frame = current_.frame;
        row_stride_bytes = static_cast<qsizetype>(coverage->row_stride_bytes);
    } else if (!valid_materialized_mask_coverage(payload)) {
        return false;
    }

    mask_coverage_ = {
        .frame = std::move(frame),
        .samples = std::move(payload.samples),
        .dimensions = payload.dimensions,
        .row_stride_bytes = row_stride_bytes,
        .generation = generation,
        .available = true,
    };
    return true;
}

EditPreviewStore::MaskCoverageSnapshot
EditPreviewStore::maskCoverageSnapshot(const MaskCoverageGeneration generation) const {
    QReadLocker lock(&lock_);
    if (!mask_coverage_.available || mask_coverage_.generation != generation
        || generation.paired_preview_generation != current_.generation) {
        return {};
    }
    return {
        .frame = mask_coverage_.frame,
        .samples = mask_coverage_.samples,
        .dimensions = mask_coverage_.dimensions,
        .row_stride_bytes = mask_coverage_.row_stride_bytes,
    };
}
