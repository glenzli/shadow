#pragma once

#include <shadow/image/optics.hpp>
#include <shadow/image/photo_geometry.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image::lensfun_profile_catalog {
struct Match;
}

namespace shadow::image::detail {
class PreparedSceneLinearRegionOptics;
}

namespace shadow::image::detail::lensfun_modifier_plan {

inline constexpr std::string_view region_executor_identity = "shadow-lensfun-region-cpu-v1";

class LensfunModifierPlan;

// One allocation identifies the concrete prepared optics plan that owns a region contract.
// Construction is intentionally unavailable to callers: pointer identity is meaningful only when
// issued by a Lensfun plan or the neutral/manual prepared-optics owner.
class PreparedRegionPlanIdentity final {
  private:
    PreparedRegionPlanIdentity() = default;

    friend class LensfunModifierPlan;
    friend class shadow::image::detail::PreparedSceneLinearRegionOptics;
};

// Immutable region evidence prepared in full-image coordinates. The coordinate table stores
// absolute (x, y) pairs for R, G, and B in that order. Keeping them absolute until sampling is
// intentional: floor-before-origin-subtraction preserves Lensfun's full-frame boundary behavior.
class PreparedRegion final {
  public:
    PreparedRegion(const PreparedRegion&) = default;
    PreparedRegion& operator=(const PreparedRegion&) = default;
    PreparedRegion(PreparedRegion&&) noexcept = default;
    PreparedRegion& operator=(PreparedRegion&&) noexcept = default;
    ~PreparedRegion() = default;

    [[nodiscard]] GeometryPixelRect output_rect() const noexcept;
    [[nodiscard]] const std::optional<GeometryPixelRect>& source_preimage() const noexcept;
    [[nodiscard]] const std::vector<float>& absolute_source_coordinates() const noexcept;
    [[nodiscard]] const std::vector<float>& profile_vignetting_gains() const noexcept;
    [[nodiscard]] const OpticsSettings& manual_output_settings() const noexcept;
    [[nodiscard]] std::string_view executor_identity() const noexcept;
    [[nodiscard]] bool coordinate_remap() const noexcept;
    [[nodiscard]] bool profile_vignetting() const noexcept;

    [[nodiscard]] bool all_out_of_bounds() const noexcept {
        return coordinate_remap_ && !source_preimage_.has_value();
    }

  private:
    PreparedRegion(
        GeometryPixelRect output_rect,
        std::optional<GeometryPixelRect> source_preimage,
        std::vector<float> absolute_source_coordinates,
        std::vector<float> profile_vignetting_gains,
        OpticsSettings manual_output_settings,
        bool coordinate_remap,
        bool profile_vignetting,
        std::shared_ptr<const PreparedRegionPlanIdentity> plan_identity
    );

    GeometryPixelRect output_rect_;
    std::optional<GeometryPixelRect> source_preimage_;
    std::vector<float> absolute_source_coordinates_;
    std::vector<float> profile_vignetting_gains_;
    OpticsSettings manual_output_settings_;
    bool coordinate_remap_ = false;
    bool profile_vignetting_ = false;
    std::shared_ptr<const PreparedRegionPlanIdentity> plan_identity_;

    friend class LensfunModifierPlan;
    friend class shadow::image::detail::PreparedSceneLinearRegionOptics;
};

// Owns cloned Lensfun camera/lens profiles plus every scalar used to compile a modifier. Each
// execution creates a private modifier from those immutable inputs, so a prepared plan neither
// borrows the provider/catalog nor shares undocumented mutable Lensfun state across threads.
class LensfunModifierPlan final {
  public:
    struct Impl;

    explicit LensfunModifierPlan(std::unique_ptr<Impl> implementation);
    ~LensfunModifierPlan();

    LensfunModifierPlan(const LensfunModifierPlan&) = delete;
    LensfunModifierPlan& operator=(const LensfunModifierPlan&) = delete;
    LensfunModifierPlan(LensfunModifierPlan&&) = delete;
    LensfunModifierPlan& operator=(LensfunModifierPlan&&) = delete;

    [[nodiscard]] Dimensions full_dimensions() const noexcept;
    [[nodiscard]] const AssetMetadata& metadata() const noexcept;
    [[nodiscard]] const OpticsSettings& settings() const noexcept;
    [[nodiscard]] const OpticsProfileReceipt& receipt() const noexcept;

    [[nodiscard]] PreparedRegion prepare_region(GeometryPixelRect output_rect) const;
    [[nodiscard]] SceneLinearRgbFrame render_scene_linear_region_cpu(
        const SceneLinearRgbFrame& source_preimage,
        const PreparedRegion& region
    ) const;

    [[nodiscard]] OpticsCorrectionResult correct_reference_rgb(const PixelBuffer& input) const;
    [[nodiscard]] SceneLinearOpticsCorrectionResult
    correct_scene_linear_reference(const SceneLinearRgbFrame& input) const;

  private:
    std::unique_ptr<Impl> implementation_;
    std::shared_ptr<const PreparedRegionPlanIdentity> region_plan_identity_;

    friend class shadow::image::detail::PreparedSceneLinearRegionOptics;
};

[[nodiscard]] std::shared_ptr<const LensfunModifierPlan> make_plan(
    const lensfun_profile_catalog::Match& match,
    OpticsProviderInfo provider,
    Dimensions full_dimensions,
    AssetMetadata metadata,
    OpticsSettings settings
);

} // namespace shadow::image::detail::lensfun_modifier_plan
