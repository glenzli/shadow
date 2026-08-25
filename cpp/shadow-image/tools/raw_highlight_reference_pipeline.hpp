#pragma once

#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_frame.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace shadow::image::probe_detail {

// A diagnostic-only, active-area float CFA plane produced from Shadow's provider-neutral
// RawFrame. The reconstruction follows darktable's "inpaint opposed" ownership boundary: only
// photosites at the per-channel clip value are replaced. It is deliberately kept outside the
// production renderer so comparisons cannot change desktop pixels or cache identity.
struct DarktableOpposedReferencePlane final {
    Dimensions dimensions;
    std::array<RawCfaColor, 4U> bayer_2x2{};
    std::vector<float> measured_samples;
    std::vector<float> reconstructed_samples;
    std::array<float, 3U> effective_white_balance_gains{};
    std::array<float, 3U> clip_values{};
    std::array<float, 3U> chrominance_offsets{};
    std::array<std::uint64_t, 3U> chrominance_support{};
    std::array<std::uint64_t, 3U> clipped_photosites_by_channel{};
    std::array<std::uint64_t, 3U> changed_photosites_by_channel{};
    std::uint64_t clipped_photosites = 0U;
    std::uint64_t changed_photosites = 0U;

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] DarktableOpposedReferencePlane make_darktable_opposed_reference_plane(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform
);

// Integrates one output footprint directly from the reconstructed reference CFA. This matches
// Shadow's warm-preview reduction frontier and lets the production area sampler be compared
// against the source-stage oracle without any later RGB rewrite.
[[nodiscard]] std::array<float, 3U> darktable_opposed_area_camera_rgb_at(
    const DarktableOpposedReferencePlane& plane,
    Dimensions target_dimensions,
    std::uint32_t target_x,
    std::uint32_t target_y
);

} // namespace shadow::image::probe_detail
