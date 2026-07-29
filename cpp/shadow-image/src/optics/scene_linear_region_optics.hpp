#pragma once

#include <shadow/image/optics.hpp>
#include <shadow/image/photo_geometry.hpp>

#include <memory>

namespace shadow::image::raw_pipeline_detail {
class PreparedRawFrameSource;
}

namespace shadow::image::detail {

namespace lensfun_modifier_plan {
class LensfunModifierPlan;
class PreparedRegionPlanIdentity;
class PreparedRegion;
}

// One allocation binds a prepared optics owner to the exact prepared RAW source that issued it.
// Ordinary optics preparation remains deliberately unbound; only PreparedRawFrameSource can
// create and inject this identity before a resident aggregate is published.
class PreparedRegionOpticsSourceIdentity final {
  private:
    PreparedRegionOpticsSourceIdentity() = default;

    friend class raw_pipeline_detail::PreparedRawFrameSource;
};

enum class SceneLinearRegionOpticsKind : std::uint8_t {
    neutral,
    pointwise,
    coordinate_remap,
    materialized_only,
};

// A prepared plan is a self-contained immutable execution contract. Pointwise plans may be
// retained after the provider goes out of scope; no region render borrows a provider, profile
// catalog, or Lensfun modifier.
class PreparedSceneLinearRegionOptics final {
  public:
    PreparedSceneLinearRegionOptics(
        SceneLinearRegionOpticsKind kind,
        Dimensions full_dimensions,
        OpticsSettings settings,
        OpticsProfileReceipt receipt,
        std::shared_ptr<const lensfun_modifier_plan::LensfunModifierPlan> lensfun_plan = nullptr
    );

    [[nodiscard]] SceneLinearRegionOpticsKind kind() const noexcept;
    [[nodiscard]] Dimensions full_dimensions() const noexcept;
    [[nodiscard]] const OpticsSettings& settings() const noexcept;
    [[nodiscard]] const OpticsProfileReceipt& receipt() const noexcept;
    [[nodiscard]] const std::shared_ptr<const lensfun_modifier_plan::LensfunModifierPlan>&
    lensfun_plan() const noexcept;

    [[nodiscard]] bool resident_eligible() const noexcept {
        return kind_ == SceneLinearRegionOpticsKind::neutral
               || kind_ == SceneLinearRegionOpticsKind::pointwise;
    }

    [[nodiscard]] bool has_owned_coordinate_remap() const noexcept {
        return kind_ == SceneLinearRegionOpticsKind::coordinate_remap && lensfun_plan_ != nullptr;
    }

    [[nodiscard]] bool device_resident_eligible() const noexcept {
        return kind_ == SceneLinearRegionOpticsKind::neutral
            || kind_ == SceneLinearRegionOpticsKind::pointwise
            || has_owned_coordinate_remap();
    }

    // Compile one self-contained output-region contract without making the resident RAW
    // consumer inspect provider or Lensfun state. Neutral/manual-only plans use the output
    // rectangle as their exact source preimage; owned profile plans retain their compiled
    // coordinate/vignetting evidence.
    [[nodiscard]] lensfun_modifier_plan::PreparedRegion
    prepare_region(GeometryPixelRect output_rect) const;

    [[nodiscard]] bool owns_region(
        const lensfun_modifier_plan::PreparedRegion& region
    ) const noexcept;

  private:
    SceneLinearRegionOpticsKind kind_ = SceneLinearRegionOpticsKind::materialized_only;
    Dimensions full_dimensions_;
    OpticsSettings settings_;
    OpticsProfileReceipt receipt_;
    std::shared_ptr<const lensfun_modifier_plan::LensfunModifierPlan> lensfun_plan_;
    std::shared_ptr<const lensfun_modifier_plan::PreparedRegionPlanIdentity> region_plan_identity_;
    std::shared_ptr<const PreparedRegionOpticsSourceIdentity> source_identity_;

    friend class raw_pipeline_detail::PreparedRawFrameSource;
};

// Optional private extension implemented only by providers that can prove their region locality.
// Unknown public OpticsProvider implementations deliberately fail closed to materialized_only.
class SceneLinearRegionOpticsProvider {
  public:
    virtual ~SceneLinearRegionOpticsProvider() = default;

    [[nodiscard]] virtual PreparedSceneLinearRegionOptics prepare_scene_linear_region_optics(
        Dimensions full_dimensions,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const = 0;
};

[[nodiscard]] PreparedSceneLinearRegionOptics prepare_scene_linear_region_optics(
    const OpticsProvider* provider,
    Dimensions full_dimensions,
    const AssetMetadata& metadata,
    const OpticsSettings& settings
);

// Applies only a prepared pointwise plan to one oriented output-space region. Profile vignetting
// is applied before the manual output vignette. Coordinate-remap plans expose their owned
// LensfunModifierPlan to the Phase C region executor and deliberately remain ineligible here.
void apply_scene_linear_region_optics(
    SceneLinearRgbFrame& region,
    GeometryPixelRect output_rect,
    const PreparedSceneLinearRegionOptics& optics
);

} // namespace shadow::image::detail
