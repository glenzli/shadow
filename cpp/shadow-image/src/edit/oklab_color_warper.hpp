#pragma once

#include <cstddef>

namespace shadow::image {

struct AdjustmentNode;
struct FloatRgbImage;
struct OklabColorWarperAdjustment;

namespace detail {

inline constexpr double oklab_color_warper_edge_feather = 0.04;

void validate_oklab_color_warper(const OklabColorWarperAdjustment& parameters,
                                 const AdjustmentNode& node, std::size_t node_index);

[[nodiscard]] bool
oklab_color_warper_is_neutral(const OklabColorWarperAdjustment& parameters) noexcept;

void apply_oklab_color_warper_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                  std::size_t node_index,
                                  const OklabColorWarperAdjustment& parameters);

} // namespace detail

} // namespace shadow::image
