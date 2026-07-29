#include "lensfun_modifier_plan_internal.hpp"

#include "lensfun_profile_catalog.hpp"
#include "manual_optics.hpp"

#include <shadow/image/decoder_error.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace shadow::image::detail::lensfun_modifier_plan {

namespace {

#if SHADOW_IMAGE_HAS_LENSFUN

void validate_full_dimensions(const Dimensions dimensions) {
    if (dimensions.width == 0U || dimensions.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Lensfun modifier plan requires non-empty full dimensions"
        );
    }
    if (dimensions.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || dimensions.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "Lensfun modifier plan dimensions exceed Lensfun's integer API"
        );
    }
    static_cast<void>(internal::checked_sample_count(
        dimensions.width,
        dimensions.height,
        internal::rgb_channels,
        "Lensfun modifier plan dimensions overflow the address space"
    ));
    constexpr std::size_t bytes_per_scene_linear_pixel = internal::rgb_channels * sizeof(float);
    if (static_cast<std::size_t>(dimensions.width)
        > static_cast<std::size_t>(std::numeric_limits<int>::max())
              / bytes_per_scene_linear_pixel) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "Lensfun modifier plan row stride exceeds Lensfun's integer API"
        );
    }
}

[[nodiscard]] float crop_factor(const AssetMetadata& metadata, const lfCamera& camera) noexcept {
    if (internal::finite_positive(metadata.focal_length_mm)
        && internal::finite_positive(metadata.focal_length_35mm)) {
        const double derived = metadata.focal_length_35mm / metadata.focal_length_mm;
        if (std::isfinite(derived) && derived >= 0.5 && derived <= 8.0) {
            return static_cast<float>(derived);
        }
    }
    return camera.CropFactor > 0.0F ? camera.CropFactor : 1.0F;
}

#endif

} // namespace

namespace internal {

#if SHADOW_IMAGE_HAS_LENSFUN

void CameraDeleter::operator()(lfCamera* camera) const noexcept {
    if (camera != nullptr) {
        lf_camera_destroy(camera);
    }
}

void LensDeleter::operator()(lfLens* lens) const noexcept {
    if (lens != nullptr) {
        lf_lens_destroy(lens);
    }
}

ConfiguredModifier
configure_modifier(const LensfunModifierPlan::Impl& plan, const lfPixelFormat pixel_format) {
    const bool has_aperture = finite_positive(plan.metadata.aperture_f_number);
    const bool has_distance = finite_positive(plan.metadata.focus_distance_meters);
    const bool request_vignetting = plan.settings.correct_vignetting && has_aperture;
    const float distance =
        has_distance ? static_cast<float>(plan.metadata.focus_distance_meters) : 1000.0F;

#if LF_VERSION_MICRO >= 99
    auto modifier = std::make_unique<lfModifier>(
        plan.lens.get(),
        static_cast<float>(plan.metadata.focal_length_mm),
        crop_factor(plan.metadata, *plan.camera),
        static_cast<int>(plan.full_dimensions.width),
        static_cast<int>(plan.full_dimensions.height),
        pixel_format
    );
    if (plan.settings.correct_distortion) {
        modifier->EnableDistortionCorrection();
    }
    if (plan.settings.correct_tca) {
        modifier->EnableTCACorrection();
    }
    if (request_vignetting) {
        modifier->EnableVignettingCorrection(
            static_cast<float>(plan.metadata.aperture_f_number),
            distance
        );
    }
    int flags = modifier->GetModFlags();
    const bool geometry = (flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) != 0;
    bool applied_scaling = false;
    if (plan.settings.automatic_scale && geometry) {
        const float automatic_scale = modifier->GetAutoScale(false);
        if (std::isfinite(automatic_scale) && automatic_scale > 0.0F) {
            modifier->EnableScaling(automatic_scale);
            flags = modifier->GetModFlags();
            applied_scaling = (flags & LF_MODIFY_SCALE) != 0;
        }
    }
    return {
        .modifier = std::move(modifier),
        .flags = flags,
        .applied_scaling = applied_scaling,
    };
#else
    int requested_flags = 0;
    if (plan.settings.correct_distortion) {
        requested_flags |= LF_MODIFY_DISTORTION;
    }
    if (plan.settings.correct_tca) {
        requested_flags |= LF_MODIFY_TCA;
    }
    if (request_vignetting) {
        requested_flags |= LF_MODIFY_VIGNETTING;
    }
    const bool geometry = (requested_flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) != 0;
    if (plan.settings.automatic_scale && geometry) {
        requested_flags |= LF_MODIFY_SCALE;
    }

    auto modifier = std::make_unique<lfModifier>(
        plan.lens.get(),
        crop_factor(plan.metadata, *plan.camera),
        static_cast<int>(plan.full_dimensions.width),
        static_cast<int>(plan.full_dimensions.height)
    );
    const int flags = modifier->Initialize(
        plan.lens.get(),
        pixel_format,
        static_cast<float>(plan.metadata.focal_length_mm),
        request_vignetting ? static_cast<float>(plan.metadata.aperture_f_number) : 0.0F,
        request_vignetting ? distance : 1000.0F,
        plan.settings.automatic_scale && geometry ? 0.0F : 1.0F,
        plan.lens->Type,
        requested_flags,
        false
    );
    return {
        .modifier = std::move(modifier),
        .flags = flags,
        .applied_scaling = (flags & LF_MODIFY_SCALE) != 0,
    };
#endif
}

OpticsProfileReceipt
receipt_for(const LensfunModifierPlan::Impl& plan, const ConfiguredModifier& modifier) {
    const bool uses_distance_fallback = plan.settings.correct_vignetting
                                        && finite_positive(plan.metadata.aperture_f_number)
                                        && !finite_positive(plan.metadata.focus_distance_meters)
                                        && (modifier.flags & LF_MODIFY_VIGNETTING) != 0;
    return OpticsProfileReceipt{
        .status = OpticsProfileStatus::matched,
        .provider_id = plan.provider.id,
        .provider_version = plan.provider.version,
        .camera_profile = plan.camera_profile,
        .lens_profile = plan.lens_profile,
        .distortion_available = (modifier.flags & LF_MODIFY_DISTORTION) != 0,
        .tca_available = (modifier.flags & LF_MODIFY_TCA) != 0,
        .vignetting_available = (modifier.flags & LF_MODIFY_VIGNETTING) != 0,
        .applied_distortion = (modifier.flags & LF_MODIFY_DISTORTION) != 0,
        .applied_tca = (modifier.flags & LF_MODIFY_TCA) != 0,
        .applied_vignetting = (modifier.flags & LF_MODIFY_VIGNETTING) != 0,
        .vignetting_used_distance_fallback = uses_distance_fallback,
        .applied_scaling = modifier.applied_scaling,
    };
}

#endif

[[noreturn]] void throw_lensfun_unavailable() {
    throw DecodeError(
        DecodeErrorCode::unsupported,
        0,
        "Lensfun modifier plan is unavailable because Lensfun is not linked"
    );
}

} // namespace internal

PreparedRegion::PreparedRegion(
    const GeometryPixelRect output_rect,
    std::optional<GeometryPixelRect> source_preimage,
    std::vector<float> absolute_source_coordinates,
    std::vector<float> profile_vignetting_gains,
    OpticsSettings manual_output_settings,
    const bool coordinate_remap,
    const bool profile_vignetting,
    std::shared_ptr<const PreparedRegionPlanIdentity> plan_identity
) :
    output_rect_(output_rect), source_preimage_(std::move(source_preimage)),
    absolute_source_coordinates_(std::move(absolute_source_coordinates)),
    profile_vignetting_gains_(std::move(profile_vignetting_gains)),
    manual_output_settings_(std::move(manual_output_settings)),
    coordinate_remap_(coordinate_remap), profile_vignetting_(profile_vignetting),
    plan_identity_(std::move(plan_identity)) {
    if (plan_identity_ == nullptr) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "prepared Lensfun region is missing its owner identity"
        );
    }
}

GeometryPixelRect PreparedRegion::output_rect() const noexcept {
    return output_rect_;
}

const std::optional<GeometryPixelRect>& PreparedRegion::source_preimage() const noexcept {
    return source_preimage_;
}

const std::vector<float>& PreparedRegion::absolute_source_coordinates() const noexcept {
    return absolute_source_coordinates_;
}

const std::vector<float>& PreparedRegion::profile_vignetting_gains() const noexcept {
    return profile_vignetting_gains_;
}

const OpticsSettings& PreparedRegion::manual_output_settings() const noexcept {
    return manual_output_settings_;
}

std::string_view PreparedRegion::executor_identity() const noexcept {
    return region_executor_identity;
}

bool PreparedRegion::coordinate_remap() const noexcept {
    return coordinate_remap_;
}

bool PreparedRegion::profile_vignetting() const noexcept {
    return profile_vignetting_;
}

LensfunModifierPlan::LensfunModifierPlan(std::unique_ptr<Impl> implementation) :
    implementation_(std::move(implementation)),
    region_plan_identity_(new PreparedRegionPlanIdentity()) {}

LensfunModifierPlan::~LensfunModifierPlan() = default;

Dimensions LensfunModifierPlan::full_dimensions() const noexcept {
    return implementation_->full_dimensions;
}

const AssetMetadata& LensfunModifierPlan::metadata() const noexcept {
    return implementation_->metadata;
}

const OpticsSettings& LensfunModifierPlan::settings() const noexcept {
    return implementation_->settings;
}

const OpticsProfileReceipt& LensfunModifierPlan::receipt() const noexcept {
    return implementation_->receipt;
}

std::shared_ptr<const LensfunModifierPlan> make_plan(
    const lensfun_profile_catalog::Match& match,
    OpticsProviderInfo provider,
    const Dimensions full_dimensions,
    AssetMetadata metadata,
    OpticsSettings settings
) {
#if SHADOW_IMAGE_HAS_LENSFUN
    manual_optics::validate_settings(settings);
    validate_full_dimensions(full_dimensions);
    if (match.camera == nullptr || match.lens == nullptr
        || !internal::finite_positive(metadata.focal_length_mm)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Lensfun modifier plan requires a matched profile and positive focal length"
        );
    }

    auto implementation = std::make_unique<LensfunModifierPlan::Impl>();
    implementation->full_dimensions = full_dimensions;
    implementation->metadata = std::move(metadata);
    implementation->settings = std::move(settings);
    implementation->provider = std::move(provider);
    implementation->camera_profile = match.camera_name;
    implementation->lens_profile = match.lens_name;
    implementation->camera.reset(lf_camera_new());
    implementation->lens.reset(lf_lens_new());
    if (implementation->camera == nullptr || implementation->lens == nullptr) {
        throw std::bad_alloc();
    }
    lf_camera_copy(implementation->camera.get(), match.camera);
    lf_lens_copy(implementation->lens.get(), match.lens);
    const internal::ConfiguredModifier configured =
        internal::configure_modifier(*implementation, LF_PF_F32);
    implementation->receipt = internal::receipt_for(*implementation, configured);
    return std::shared_ptr<const LensfunModifierPlan>(
        new LensfunModifierPlan(std::move(implementation))
    );
#else
    static_cast<void>(match);
    static_cast<void>(provider);
    static_cast<void>(full_dimensions);
    static_cast<void>(metadata);
    static_cast<void>(settings);
    internal::throw_lensfun_unavailable();
#endif
}

} // namespace shadow::image::detail::lensfun_modifier_plan
