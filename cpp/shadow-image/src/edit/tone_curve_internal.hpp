#pragma once

// Internal prepared-state contract shared by CPU execution and Metal program lowering.

#include <cstddef>
#include <span>
#include <variant>
#include <vector>

namespace shadow::image {

struct AdjustmentNode;
struct FloatRgbImage;
struct OklabLightnessToneCurve;
struct OklabOpponentToneCurves;
struct ToneCurveSet;

namespace detail {

struct WorkingSpaceTransform;

class PreparedSmoothToneCurve final {
  public:
    PreparedSmoothToneCurve(const ToneCurveSet& source, std::vector<double> knot_derivatives,
                            bool identity);

    [[nodiscard]] const ToneCurveSet& source() const noexcept;
    [[nodiscard]] std::span<const double> knot_derivatives() const noexcept;
    [[nodiscard]] std::size_t segment_count() const noexcept;
    [[nodiscard]] bool is_identity() const noexcept;

  private:
    const ToneCurveSet* source_ = nullptr;
    std::vector<double> knot_derivatives_;
    bool identity_ = false;
};

struct PreparedOklabOpponentToneCurves final {
    PreparedSmoothToneCurve a;
    PreparedSmoothToneCurve b;

    [[nodiscard]] bool is_identity() const noexcept { return a.is_identity() && b.is_identity(); }
};

using PreparedToneCurveAdjustment =
    std::variant<std::monostate, PreparedSmoothToneCurve, PreparedOklabOpponentToneCurves>;

[[nodiscard]] PreparedSmoothToneCurve
prepare_oklab_lightness_tone_curve_node(const OklabLightnessToneCurve& curve,
                                        const AdjustmentNode& node, std::size_t index);

[[nodiscard]] PreparedOklabOpponentToneCurves
prepare_oklab_opponent_tone_curves_node(const OklabOpponentToneCurves& curves,
                                        const AdjustmentNode& node, std::size_t index);

void apply_prepared_oklab_lightness_tone_curve(FloatRgbImage& image,
                                               const PreparedSmoothToneCurve& prepared,
                                               const WorkingSpaceTransform& color_transform,
                                               const AdjustmentNode& node, std::size_t node_index);

void apply_prepared_oklab_opponent_tone_curves(FloatRgbImage& image,
                                               const PreparedOklabOpponentToneCurves& prepared,
                                               const WorkingSpaceTransform& color_transform,
                                               const AdjustmentNode& node, std::size_t node_index);

} // namespace detail

} // namespace shadow::image
