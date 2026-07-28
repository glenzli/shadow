#pragma once

#include <cstddef>

namespace shadow::image {

struct AdjustmentFootprint;
struct AdjustmentNode;
struct FloatRgbImage;
struct SharpenAdjustment;

namespace detail {

class PreparedColorGradingWheel final {
  public:
    PreparedColorGradingWheel(double delta_a, double delta_b, double delta_lightness) noexcept;

    [[nodiscard]] double delta_a() const noexcept { return delta_a_; }
    [[nodiscard]] double delta_b() const noexcept { return delta_b_; }
    [[nodiscard]] double delta_lightness() const noexcept { return delta_lightness_; }

  private:
    double delta_a_ = 0.0;
    double delta_b_ = 0.0;
    double delta_lightness_ = 0.0;
};

class PreparedColorGrading final {
  public:
    PreparedColorGrading(PreparedColorGradingWheel shadows, PreparedColorGradingWheel midtones,
                         PreparedColorGradingWheel highlights, double center, double width,
                         bool identity) noexcept;

    [[nodiscard]] const PreparedColorGradingWheel& shadows() const noexcept { return shadows_; }
    [[nodiscard]] const PreparedColorGradingWheel& midtones() const noexcept { return midtones_; }
    [[nodiscard]] const PreparedColorGradingWheel& highlights() const noexcept {
        return highlights_;
    }
    [[nodiscard]] double center() const noexcept { return center_; }
    [[nodiscard]] double width() const noexcept { return width_; }
    [[nodiscard]] bool is_identity() const noexcept { return identity_; }

  private:
    PreparedColorGradingWheel shadows_;
    PreparedColorGradingWheel midtones_;
    PreparedColorGradingWheel highlights_;
    double center_ = 0.5;
    double width_ = 0.23;
    bool identity_ = true;
};

[[nodiscard]] PreparedColorGrading
prepare_color_grading(const SharpenAdjustment& parameters) noexcept;

[[nodiscard]] AdjustmentFootprint creative_detail_footprint(const SharpenAdjustment& parameters,
                                                            double level_zero_to_raster_scale_x,
                                                            double level_zero_to_raster_scale_y);

// Texture, clarity, and local contrast deliberately share one Oklab field and
// final write before the prepared grading wheels are applied.
void apply_creative_detail_grading_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                       std::size_t node_index, const SharpenAdjustment& parameters);

} // namespace detail

} // namespace shadow::image
