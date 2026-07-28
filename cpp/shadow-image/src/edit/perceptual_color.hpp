#pragma once

#include <cstddef>
#include <span>

namespace shadow::image {

struct AdjustmentNode;
struct FloatRgbImage;
struct PerceptualColorAdjustment;

namespace detail {

class PerceptualColorStages final {
  public:
    [[nodiscard]] constexpr bool hue_mapping_active() const noexcept { return hue_mapping_; }
    [[nodiscard]] constexpr bool opponent_balance_active() const noexcept {
        return opponent_balance_;
    }
    [[nodiscard]] constexpr bool selective_color_active() const noexcept {
        return selective_color_;
    }
    [[nodiscard]] constexpr bool oklab_pipeline_active() const noexcept {
        return hue_mapping_ || opponent_balance_;
    }
    [[nodiscard]] constexpr std::size_t active_stage_count() const noexcept {
        return static_cast<std::size_t>(hue_mapping_) +
               static_cast<std::size_t>(opponent_balance_) +
               static_cast<std::size_t>(selective_color_);
    }
    [[nodiscard]] constexpr bool neutral() const noexcept { return active_stage_count() == 0U; }

  private:
    friend PerceptualColorStages
    classify_perceptual_color(const PerceptualColorAdjustment& parameters) noexcept;

    constexpr PerceptualColorStages(const bool hue_mapping, const bool opponent_balance,
                                    const bool selective_color) noexcept
        : hue_mapping_(hue_mapping), opponent_balance_(opponent_balance),
          selective_color_(selective_color) {}

    bool hue_mapping_ = false;
    bool opponent_balance_ = false;
    bool selective_color_ = false;
};

[[nodiscard]] PerceptualColorStages
classify_perceptual_color(const PerceptualColorAdjustment& parameters) noexcept;

[[nodiscard]] std::span<const double> perceptual_color_hue_anchors() noexcept;

void validate_perceptual_color(const PerceptualColorAdjustment& parameters,
                               const AdjustmentNode& node, std::size_t node_index);

void apply_perceptual_color_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                std::size_t node_index, const PerceptualColorAdjustment& parameters,
                                PerceptualColorStages stages);

} // namespace detail

} // namespace shadow::image
