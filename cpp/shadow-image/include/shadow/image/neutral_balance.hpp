#pragma once

#include <shadow/image/edit.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace shadow::image {

// A conservative, scene-linear neutral diagnostic. It identifies pixels that
// are already close to the neutral axis and reports their robust RGB ratios.
// The result intentionally never mutates a white-balance node: an editor must
// show the evidence and let the user confirm a suggested correction.
struct NeutralBalanceAnalysisOptions final {
    double minimum_luminance = 0.02;
    double maximum_luminance = 0.95;
    double maximum_relative_chroma = 0.12;
    std::size_t maximum_candidate_count = 5U;
};

struct NeutralBalanceCandidate final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double luminance = 0.0;
    // max(rgb) - min(rgb), normalized by max(rgb). It is a diagnostic
    // measurement rather than an Oklab chroma value.
    double relative_chroma = 0.0;
    double confidence = 0.0;
};

struct NeutralBalanceAnalysis final {
    bool available = false;
    std::uint64_t sampled_pixel_count = 0U;
    std::uint64_t eligible_pixel_count = 0U;
    // Robust geometric RGB ratios of the accepted near-neutral population.
    // Values above one indicate more red/blue than green in those samples.
    double red_to_green = 1.0;
    double blue_to_green = 1.0;
    // Inverse ratios are deliberately supplied as evidence for a later,
    // user-confirmed correction. They are not auto-applied by this module.
    double suggested_red_gain = 1.0;
    double suggested_blue_gain = 1.0;
    double confidence = 0.0;
    std::vector<NeutralBalanceCandidate> candidates;
};

// Requires an interleaved, finite scene-linear RGB raster. Malformed image
// metadata or analysis options throw std::invalid_argument; an ordinary image
// without enough credible neutral evidence returns `available == false`.
[[nodiscard]] NeutralBalanceAnalysis analyze_scene_linear_neutral_balance(
    const FloatRgbImage& image,
    NeutralBalanceAnalysisOptions options = {}
);

} // namespace shadow::image
