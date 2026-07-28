#pragma once

#include <cstddef>
#include <cstdint>

namespace shadow::image {

struct AdjustmentFootprint;
struct AdjustmentNode;
struct FloatRgbImage;
struct SelectiveToneAdjustment;

namespace detail {

// One validated snapshot binds the authored EV amounts to both guided-filter
// radii and the complete two-pass scheduler support.
class PreparedGuidedSelectiveTone final {
  public:
    [[nodiscard]] bool neutral() const noexcept { return neutral_; }
    [[nodiscard]] double highlights() const noexcept { return highlights_; }
    [[nodiscard]] double shadows() const noexcept { return shadows_; }
    [[nodiscard]] double whites() const noexcept { return whites_; }
    [[nodiscard]] double blacks() const noexcept { return blacks_; }
    [[nodiscard]] std::uint32_t mask_radius_x() const noexcept { return mask_radius_x_; }
    [[nodiscard]] std::uint32_t mask_radius_y() const noexcept { return mask_radius_y_; }
    [[nodiscard]] AdjustmentFootprint footprint() const noexcept;

  private:
    PreparedGuidedSelectiveTone(bool neutral, double highlights, double shadows, double whites,
                                double blacks, std::uint32_t mask_radius_x,
                                std::uint32_t mask_radius_y, std::uint32_t support_radius_x,
                                std::uint32_t support_radius_y) noexcept;

    friend PreparedGuidedSelectiveTone
    prepare_guided_selective_tone(const SelectiveToneAdjustment& parameters,
                                  double level_zero_to_raster_scale_x,
                                  double level_zero_to_raster_scale_y);

    bool neutral_ = true;
    double highlights_ = 0.0;
    double shadows_ = 0.0;
    double whites_ = 0.0;
    double blacks_ = 0.0;
    std::uint32_t mask_radius_x_ = 0U;
    std::uint32_t mask_radius_y_ = 0U;
    std::uint32_t support_radius_x_ = 0U;
    std::uint32_t support_radius_y_ = 0U;
};

void validate_guided_selective_tone(const SelectiveToneAdjustment& parameters,
                                    const AdjustmentNode& node, std::size_t node_index);

[[nodiscard]] bool
guided_selective_tone_is_neutral(const SelectiveToneAdjustment& parameters) noexcept;

[[nodiscard]] PreparedGuidedSelectiveTone
prepare_guided_selective_tone(const SelectiveToneAdjustment& parameters,
                              double level_zero_to_raster_scale_x,
                              double level_zero_to_raster_scale_y);

void apply_prepared_guided_selective_tone_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                              std::size_t node_index,
                                              const PreparedGuidedSelectiveTone& prepared);

} // namespace detail

} // namespace shadow::image
