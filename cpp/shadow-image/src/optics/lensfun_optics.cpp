#include <shadow/image/optics.hpp>

#include <shadow/image/decoder_error.hpp>

#include "lensfun_modifier_plan.hpp"
#include "lensfun_profile_catalog.hpp"
#include "manual_optics.hpp"
#include "scene_linear_region_optics.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace shadow::image {

namespace {

using detail::manual_optics::has_manual_geometry;
using detail::manual_optics::has_manual_vignetting;
using detail::manual_optics::validate_manual_scene_linear_input;
using detail::manual_optics::validate_settings;
using detail::manual_optics::with_manual_optics;

[[nodiscard]] bool finite_positive(const double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] OpticsProfileReceipt
unavailable_receipt(const OpticsProviderInfo& info, const OpticsProfileStatus status) {
    return OpticsProfileReceipt{
        .status = status,
        .provider_id = info.id,
        .provider_version = info.version,
    };
}

[[nodiscard]] bool reference_layout_supported(const PixelBuffer& input) noexcept {
    return input.bits_per_channel == 16U && input.channels == 3U
           && input.transfer_function == RgbTransferFunction::linear
           && input.primaries == RgbPrimaries::srgb_rec709_d65
           && input.reference == RgbBufferReference::processed_raw;
}

void validate_reference_layout(const PixelBuffer& input) {
    const auto width = static_cast<std::size_t>(input.dimensions.width);
    const auto height = static_cast<std::size_t>(input.dimensions.height);
    constexpr std::size_t rgb_channels = 3U;
    if (input.dimensions.width == 0U || input.dimensions.height == 0U
        || width > std::numeric_limits<std::size_t>::max() / rgb_channels
        || height > std::numeric_limits<std::size_t>::max() / (width * rgb_channels)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "optics input dimensions overflow the address space"
        );
    }
    const std::size_t expected_samples = width * height * rgb_channels;
    if (input.row_stride_bytes != width * rgb_channels * sizeof(std::uint16_t)
        || input.samples.size() != expected_samples) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "optics input layout does not match its RGB descriptor"
        );
    }
}

[[nodiscard]] detail::SceneLinearRegionOpticsKind
fallback_region_kind(const OpticsSettings& settings) noexcept {
    if (has_manual_geometry(settings)) {
        return detail::SceneLinearRegionOpticsKind::coordinate_remap;
    }
    return has_manual_vignetting(settings) ? detail::SceneLinearRegionOpticsKind::pointwise
                                           : detail::SceneLinearRegionOpticsKind::neutral;
}

struct ResolvedModifierPlan final {
    OpticsProfileReceipt receipt;
    std::shared_ptr<const detail::lensfun_modifier_plan::LensfunModifierPlan> plan;
};

class LensfunOpticsProvider final : public OpticsProvider,
                                    public detail::SceneLinearRegionOpticsProvider {
  public:
    explicit LensfunOpticsProvider(std::optional<std::filesystem::path> database_directory) :
        profile_catalog_(std::move(database_directory)) {}

    [[nodiscard]] const OpticsProviderInfo& info() const noexcept override {
        return profile_catalog_.info();
    }

    [[nodiscard]] std::vector<OpticsProfileCandidate>
    profile_candidates(const AssetMetadata& metadata) const override {
        return profile_catalog_.profile_candidates(metadata);
    }

    [[nodiscard]] detail::PreparedSceneLinearRegionOptics prepare_scene_linear_region_optics(
        const Dimensions full_dimensions,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const override {
        validate_settings(settings);
        ResolvedModifierPlan resolved = resolve_plan(full_dimensions, metadata, settings);
        detail::SceneLinearRegionOpticsKind kind = fallback_region_kind(settings);
        if (resolved.plan != nullptr) {
            const OpticsProfileReceipt& receipt = resolved.plan->receipt();
            if (has_manual_geometry(settings)) {
                // Manual residual geometry is a second remap after Lensfun. Phase C-a makes the
                // profile remap independently ownable but does not pretend that composition is
                // already represented by one exact preimage.
                kind = detail::SceneLinearRegionOpticsKind::materialized_only;
            } else if (receipt.applied_distortion || receipt.applied_tca) {
                kind = detail::SceneLinearRegionOpticsKind::coordinate_remap;
            } else if (receipt.applied_vignetting || has_manual_vignetting(settings)) {
                kind = detail::SceneLinearRegionOpticsKind::pointwise;
            } else {
                kind = detail::SceneLinearRegionOpticsKind::neutral;
            }
        }
        return detail::PreparedSceneLinearRegionOptics(
            kind,
            full_dimensions,
            settings,
            std::move(resolved.receipt),
            std::move(resolved.plan)
        );
    }

    [[nodiscard]] OpticsCorrectionResult correct_reference_rgb(
        const PixelBuffer& input,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const override {
        validate_settings(settings);
        const bool could_resolve_profile =
            settings.enabled && profile_catalog_.info().available
            && finite_positive(metadata.focal_length_mm)
            && profile_catalog_.profile_identity_available(metadata, settings);
        if (could_resolve_profile && !reference_layout_supported(input)) {
            return with_manual_optics(
                unavailable_receipt(
                    profile_catalog_.info(),
                    OpticsProfileStatus::incompatible_input
                ),
                input,
                settings
            );
        }
        if (could_resolve_profile) {
            validate_reference_layout(input);
        }
        ResolvedModifierPlan resolved = resolve_plan(input.dimensions, metadata, settings);
        if (resolved.plan == nullptr) {
            return with_manual_optics(std::move(resolved.receipt), input, settings);
        }
        return resolved.plan->correct_reference_rgb(input);
    }

    [[nodiscard]] SceneLinearOpticsCorrectionResult correct_scene_linear_reference(
        const SceneLinearRgbFrame& input,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const override {
        validate_settings(settings);
        const bool could_resolve_profile =
            settings.enabled && profile_catalog_.info().available
            && finite_positive(metadata.focal_length_mm)
            && profile_catalog_.profile_identity_available(metadata, settings);
        if (could_resolve_profile) {
            validate_manual_scene_linear_input(input);
        }
        ResolvedModifierPlan resolved = resolve_plan(input.dimensions, metadata, settings);
        if (resolved.plan == nullptr) {
            return with_manual_optics(std::move(resolved.receipt), input, settings);
        }
        return resolved.plan->correct_scene_linear_reference(input);
    }

  private:
    [[nodiscard]] ResolvedModifierPlan resolve_plan(
        const Dimensions full_dimensions,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const {
        if (!settings.enabled) {
            return {
                .receipt =
                    unavailable_receipt(profile_catalog_.info(), OpticsProfileStatus::disabled),
            };
        }
        if (!profile_catalog_.info().available) {
            return {
                .receipt = unavailable_receipt(
                    profile_catalog_.info(),
                    OpticsProfileStatus::provider_unavailable
                ),
            };
        }
        if (!finite_positive(metadata.focal_length_mm)
            || !profile_catalog_.profile_identity_available(metadata, settings)) {
            return {
                .receipt = unavailable_receipt(
                    profile_catalog_.info(),
                    OpticsProfileStatus::insufficient_metadata
                ),
            };
        }
        const lensfun_profile_catalog::Resolution resolution =
            profile_catalog_.resolve_profile(metadata, settings);
        if (!resolution.match.has_value()) {
            return {
                .receipt = unavailable_receipt(profile_catalog_.info(), resolution.status),
            };
        }
        auto plan = detail::lensfun_modifier_plan::make_plan(
            *resolution.match,
            profile_catalog_.info(),
            full_dimensions,
            metadata,
            settings
        );
        return {
            .receipt = plan->receipt(),
            .plan = std::move(plan),
        };
    }

    lensfun_profile_catalog::Catalog profile_catalog_;
};

} // namespace

OpticsSettings default_optics_settings() noexcept {
    return {};
}

std::string optics_settings_signature(const OpticsSettings& settings) {
    validate_settings(settings);
    std::ostringstream signature;
    signature << "shadow-optics-v" << optics_implementation_version
              << ";enabled=" << (settings.enabled ? 1 : 0)
              << ";distortion=" << (settings.correct_distortion ? 1 : 0)
              << ";tca=" << (settings.correct_tca ? 1 : 0)
              << ";vignetting=" << (settings.correct_vignetting ? 1 : 0)
              << ";auto-scale=" << (settings.automatic_scale ? 1 : 0)
              << ";manual-distortion=" << settings.manual_distortion
              << ";manual-tca-red-cyan=" << settings.manual_tca_red_cyan
              << ";manual-tca-blue-yellow=" << settings.manual_tca_blue_yellow
              << ";manual-vignetting=" << settings.manual_vignetting_amount
              << ";manual-vignetting-midpoint="
              << static_cast<unsigned int>(settings.manual_vignetting_midpoint);
    signature << ";camera-maker=" << settings.camera_profile_maker
              << ";camera-model=" << settings.camera_profile_model
              << ";lens-maker=" << settings.lens_profile_maker
              << ";lens-model=" << settings.lens_profile_model;
    return signature.str();
}

std::shared_ptr<const OpticsProvider>
make_lensfun_optics_provider(std::optional<std::filesystem::path> database_directory) {
    return std::make_shared<LensfunOpticsProvider>(std::move(database_directory));
}

} // namespace shadow::image
