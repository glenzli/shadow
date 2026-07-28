#pragma once

#include <cstddef>

namespace shadow::image {

struct AdjustmentNode;
struct ContrastAdjustment;
struct FloatRgbImage;

namespace detail {

// Validated parameter-derived state shared by CPU execution and Metal lowering.
// Keeping construction private prevents either backend from independently
// reinterpreting the public multiplicative factor and scene-linear pivot.
class PreparedPerceptualContrast final {
  public:
    [[nodiscard]] bool neutral() const noexcept { return neutral_; }
    [[nodiscard]] double pivot_lightness() const noexcept { return pivot_lightness_; }
    [[nodiscard]] double signed_amount() const noexcept { return signed_amount_; }
    [[nodiscard]] bool collapses_to_pivot() const noexcept { return collapses_to_pivot_; }

  private:
    PreparedPerceptualContrast(bool neutral, double pivot_lightness, double signed_amount,
                              bool collapses_to_pivot) noexcept;

    friend PreparedPerceptualContrast
    prepare_perceptual_contrast(const ContrastAdjustment& parameters, const AdjustmentNode& node,
                                std::size_t node_index);

    bool neutral_ = true;
    double pivot_lightness_ = 0.0;
    double signed_amount_ = 0.0;
    bool collapses_to_pivot_ = false;
};

[[nodiscard]] PreparedPerceptualContrast
prepare_perceptual_contrast(const ContrastAdjustment& parameters, const AdjustmentNode& node,
                            std::size_t node_index);

void apply_prepared_perceptual_contrast_cpu(FloatRgbImage& image, const AdjustmentNode& node,
                                            std::size_t node_index,
                                            const PreparedPerceptualContrast& prepared);

} // namespace detail

} // namespace shadow::image
