#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>

#include <compare>
#include <cstdint>

inline constexpr std::uint32_t EDIT_MASK_COVERAGE_VERSION = 2;

// One optional coverage request belongs to the immutable Grade Stack snapshot
// handed to the preview worker. The selected node index is meaningful only
// together with the monotonic selection revision.
struct EditMaskCoverageRequest final {
    std::uint32_t target_layer_index = 0;
    // -1 requests the final composed node mask. A non-negative index requests
    // one leaf so component editing never mistakes final coverage for the
    // selected selector's geometry.
    std::int32_t target_component_index = -1;
    std::uint64_t selection_revision = 0;

    auto operator<=>(const EditMaskCoverageRequest&) const = default;
};

// Coverage is a transient projection paired with one exact current-preview
// publication. Recipe identity and preview publication identity are kept
// separate so selecting another node never pretends to mutate the Recipe.
struct MaskCoverageGeneration final {
    std::uint64_t photo = 0;
    std::uint64_t recipe_revision = 0;
    std::uint32_t target_layer_index = 0;
    std::int32_t target_component_index = -1;
    std::uint64_t selection_revision = 0;
    std::uint64_t paired_preview_generation = 0;

    auto operator<=>(const MaskCoverageGeneration&) const = default;
};

struct EditMaskCoveragePayload final {
    QByteArray samples;
    QSize dimensions;
    std::uint32_t row_stride_bytes = 0;
    std::uint32_t version = 0;
    std::uint32_t target_layer_index = 0;
    std::int32_t target_component_index = -1;
    std::uint64_t selection_revision = 0;
};

[[nodiscard]] constexpr bool accepts_mask_coverage(
    const MaskCoverageGeneration result,
    const MaskCoverageGeneration expected
) noexcept {
    return result == expected;
}

// Every selected-mask mutation must hide the renderer's previous coverage
// before the replacement preview is scheduled. Ordinary Grade Node edits keep
// the paired coverage because their mask geometry did not change.
[[nodiscard]] inline bool
invalidates_mask_coverage_for_edit(const QString& parameter_key) noexcept {
    return parameter_key.startsWith(QStringLiteral("local_mask/"));
}
