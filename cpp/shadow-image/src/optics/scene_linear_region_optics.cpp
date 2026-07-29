#include "scene_linear_region_optics.hpp"

#include "lensfun_modifier_plan.hpp"
#include "manual_optics.hpp"

#include <shadow/image/decoder_error.hpp>

#include <utility>

namespace shadow::image::detail {

namespace {

[[nodiscard]] bool rect_inside(
    const GeometryPixelRect rect,
    const Dimensions dimensions
) noexcept {
    return rect.width != 0U && rect.height != 0U && rect.x < dimensions.width
        && rect.y < dimensions.height && rect.width <= dimensions.width - rect.x
        && rect.height <= dimensions.height - rect.y;
}

} // namespace

PreparedSceneLinearRegionOptics::PreparedSceneLinearRegionOptics(
    const SceneLinearRegionOpticsKind prepared_kind,
    const Dimensions prepared_full_dimensions,
    OpticsSettings prepared_settings,
    OpticsProfileReceipt prepared_receipt,
    std::shared_ptr<const lensfun_modifier_plan::LensfunModifierPlan> prepared_lensfun_plan
) :
    kind_(prepared_kind), full_dimensions_(prepared_full_dimensions),
    settings_(std::move(prepared_settings)), receipt_(std::move(prepared_receipt)),
    lensfun_plan_(std::move(prepared_lensfun_plan)) {
    region_plan_identity_ =
        lensfun_plan_ != nullptr
            ? lensfun_plan_->region_plan_identity_
            : std::shared_ptr<const lensfun_modifier_plan::PreparedRegionPlanIdentity>(
                  new lensfun_modifier_plan::PreparedRegionPlanIdentity()
              );
}

SceneLinearRegionOpticsKind PreparedSceneLinearRegionOptics::kind() const noexcept {
    return kind_;
}

Dimensions PreparedSceneLinearRegionOptics::full_dimensions() const noexcept {
    return full_dimensions_;
}

const OpticsSettings& PreparedSceneLinearRegionOptics::settings() const noexcept {
    return settings_;
}

const OpticsProfileReceipt& PreparedSceneLinearRegionOptics::receipt() const noexcept {
    return receipt_;
}

const std::shared_ptr<const lensfun_modifier_plan::LensfunModifierPlan>&
PreparedSceneLinearRegionOptics::lensfun_plan() const noexcept {
    return lensfun_plan_;
}

lensfun_modifier_plan::PreparedRegion
PreparedSceneLinearRegionOptics::prepare_region(const GeometryPixelRect output_rect) const {
    if (!rect_inside(output_rect, full_dimensions_)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "prepared region optics received an output rectangle outside the source"
        );
    }
    if (lensfun_plan_ != nullptr) {
        auto region = lensfun_plan_->prepare_region(output_rect);
        if (!owns_region(region)) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "prepared region optics no longer owns its retained Lensfun plan"
            );
        }
        return region;
    }
    if (kind_ != SceneLinearRegionOpticsKind::neutral
        && kind_ != SceneLinearRegionOpticsKind::pointwise) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "prepared region optics has no owned regional executor"
        );
    }
    if (manual_optics::has_manual_geometry(settings_)) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "resident region optics supports manual output vignetting but not manual geometry"
        );
    }
    return lensfun_modifier_plan::PreparedRegion(
        output_rect,
        output_rect,
        {},
        {},
        settings_,
        false,
        false,
        region_plan_identity_
    );
}

bool PreparedSceneLinearRegionOptics::owns_region(
    const lensfun_modifier_plan::PreparedRegion& region
) const noexcept {
    return region_plan_identity_ != nullptr
        && region.plan_identity_.get() == region_plan_identity_.get();
}

PreparedSceneLinearRegionOptics prepare_scene_linear_region_optics(
    const OpticsProvider* provider,
    const Dimensions full_dimensions,
    const AssetMetadata& metadata,
    const OpticsSettings& settings
) {
    manual_optics::validate_settings(settings);
    if (full_dimensions.width == 0U || full_dimensions.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "region optics requires non-empty full dimensions"
        );
    }
    if (provider == nullptr) {
        return PreparedSceneLinearRegionOptics(
            SceneLinearRegionOpticsKind::neutral,
            full_dimensions,
            settings,
            {
                .status = OpticsProfileStatus::disabled,
                .provider_id = "none",
                .provider_version = "none",
            },
            nullptr
        );
    }
    const auto* region_provider = dynamic_cast<const SceneLinearRegionOpticsProvider*>(provider);
    if (region_provider == nullptr) {
        return PreparedSceneLinearRegionOptics(
            SceneLinearRegionOpticsKind::materialized_only,
            full_dimensions,
            settings,
            {
                .status = OpticsProfileStatus::incompatible_input,
                .provider_id = provider->info().id,
                .provider_version = provider->info().version,
            },
            nullptr
        );
    }
    PreparedSceneLinearRegionOptics prepared =
        region_provider->prepare_scene_linear_region_optics(full_dimensions, metadata, settings);
    if (prepared.full_dimensions() != full_dimensions) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "region optics provider changed the prepared full dimensions"
        );
    }
    return prepared;
}

void apply_scene_linear_region_optics(
    SceneLinearRgbFrame& region,
    const GeometryPixelRect output_rect,
    const PreparedSceneLinearRegionOptics& optics
) {
    if (region.dimensions != Dimensions{output_rect.width, output_rect.height}
        || output_rect.width == 0U || output_rect.height == 0U
        || output_rect.x >= optics.full_dimensions().width
        || output_rect.y >= optics.full_dimensions().height
        || output_rect.width > optics.full_dimensions().width - output_rect.x
        || output_rect.height > optics.full_dimensions().height - output_rect.y) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "region optics received inconsistent local and full-image geometry"
        );
    }
    switch (optics.kind()) {
    case SceneLinearRegionOpticsKind::neutral:
        return;
    case SceneLinearRegionOpticsKind::pointwise:
        if (optics.lensfun_plan() != nullptr) {
            const auto plan = optics.lensfun_plan()->prepare_region(output_rect);
            region = optics.lensfun_plan()->render_scene_linear_region_cpu(region, plan);
            return;
        }
        manual_optics::apply_manual_scene_linear_vignetting_region(
            region,
            optics.full_dimensions(),
            output_rect.x,
            output_rect.y,
            optics.settings()
        );
        return;
    case SceneLinearRegionOpticsKind::coordinate_remap:
    case SceneLinearRegionOpticsKind::materialized_only:
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "non-local optics cannot execute from a resident RAW region"
        );
    }
}

} // namespace shadow::image::detail
