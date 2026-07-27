#pragma once

#include <shadow/image/decoder_metadata.hpp>
#include <shadow/image/raw_development.hpp>
#include <shadow/image/reference_pixels.hpp>

#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace shadow::image {

// Lens correction is an input transform, not a creative Grade Node operation.  It runs after a
// RAW provider has produced its linear reference RGB and before Shadow materializes either a warm
// preview or a 1:1 detail source.  Keeping this boundary explicit prevents a detail tile from
// sampling its own already-cropped pixels when distortion/TCA require coordinates elsewhere in
// the complete image.
inline constexpr std::uint32_t optics_settings_schema_version = 1U;
inline constexpr std::uint32_t optics_implementation_version = 1U;

struct OpticsSettings final {
    std::uint32_t schema_version = optics_settings_schema_version;
    bool enabled = true;
    bool correct_distortion = true;
    bool correct_tca = true;
    bool correct_vignetting = true;
    // Lensfun's automatic scale avoids empty corners after geometry correction.  It is an
    // explicit part of cache identity: a photographer may intentionally turn it off to retain
    // the largest field of view while accepting transparent/black corners at export.
    bool automatic_scale = true;
    // Profile correction is a baseline. These signed integer controls are an
    // always-available residual pass: they work above a Lensfun match and on
    // images for which no profile can be resolved. Integer percent units keep
    // input-stage Recipe settings exactly comparable and hashable.
    std::int16_t manual_distortion = 0;
    std::int16_t manual_tca_red_cyan = 0;
    std::int16_t manual_tca_blue_yellow = 0;
    std::int16_t manual_vignetting_amount = 0;
    std::uint8_t manual_vignetting_midpoint = 50;
    // Empty models select metadata-driven automatic matching. A manual
    // selection stores stable Lensfun maker/model identities, never a list index.
    std::string camera_profile_maker;
    std::string camera_profile_model;
    std::string lens_profile_maker;
    std::string lens_profile_model;

    auto operator<=>(const OpticsSettings&) const = default;
};

[[nodiscard]] OpticsSettings default_optics_settings() noexcept;
[[nodiscard]] std::string optics_settings_signature(const OpticsSettings& settings);

enum class OpticsProfileStatus : std::uint8_t {
    disabled,
    provider_unavailable,
    insufficient_metadata,
    camera_not_found,
    lens_not_found,
    incompatible_input,
    matched,
};

// This is intentionally a receipt rather than a mutable profile object.  It can be persisted
// with a render cache entry or shown by the UI without coupling Recipe data to Lensfun's internal
// database structures.
struct OpticsProfileReceipt final {
    OpticsProfileStatus status = OpticsProfileStatus::provider_unavailable;
    std::string provider_id;
    std::string provider_version;
    std::string camera_profile;
    std::string lens_profile;
    bool distortion_available = false;
    bool tca_available = false;
    bool vignetting_available = false;
    bool applied_distortion = false;
    bool applied_tca = false;
    bool applied_vignetting = false;
    // RAW files frequently omit a portable focus-distance field. Lensfun's vignetting
    // calibration is still useful at normal/long focus distances, so the provider may apply it
    // with an explicit far-distance approximation. This makes that approximation auditable
    // instead of silently pretending an unavailable maker-note value was exact.
    bool vignetting_used_distance_fallback = false;
    bool applied_scaling = false;
};

struct OpticsProfileCandidate final {
    std::string camera_maker;
    std::string camera_model;
    std::string lens_maker;
    std::string lens_model;
};

struct OpticsCorrectionResult final {
    OpticsProfileReceipt receipt;
    // Empty means that input pixels are already the correct source: this avoids copying an
    // entire full-resolution image merely to report an absent profile or a disabled correction.
    std::optional<PixelBuffer> corrected_reference_rgb;
};

// Equivalent result for Shadow-owned RawFrame development.  Unlike PixelBuffer this carries
// unbounded scene-linear fp32 samples, so a lens profile must not turn recoverable sensor
// highlight headroom into a 16-bit display-white clip simply to perform a geometric remap.
struct SceneLinearOpticsCorrectionResult final {
    OpticsProfileReceipt receipt;
    // Empty means that input samples already are the appropriate source.  This mirrors the
    // packed result and avoids duplicating a full RAW frame for disabled or unmatched profiles.
    std::optional<SceneLinearRgbFrame> corrected_scene_linear_rgb;
};

struct OpticsProviderInfo final {
    std::string id;
    std::string version;
    bool available = false;
};

class OpticsProvider {
public:
    OpticsProvider() = default;
    OpticsProvider(const OpticsProvider&) = delete;
    OpticsProvider& operator=(const OpticsProvider&) = delete;
    OpticsProvider(OpticsProvider&&) = delete;
    OpticsProvider& operator=(OpticsProvider&&) = delete;
    virtual ~OpticsProvider() = default;

    [[nodiscard]] virtual const OpticsProviderInfo& info() const noexcept = 0;
    [[nodiscard]] virtual OpticsCorrectionResult correct_reference_rgb(
        const PixelBuffer& input,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const = 0;
    // Existing third-party optics adapters may only understand packed provider RGB.  Preserve
    // their source compatibility: the default explicitly declines a scene-linear frame rather
    // than forcing a lossy conversion.  Lensfun and any future float-native provider override
    // this method.
    [[nodiscard]] virtual SceneLinearOpticsCorrectionResult correct_scene_linear_reference(
        const SceneLinearRgbFrame& input,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const {
        (void)input;
        (void)metadata;
        (void)settings;
        return SceneLinearOpticsCorrectionResult{
            .receipt = {
                .status = OpticsProfileStatus::incompatible_input,
                .provider_id = info().id,
                .provider_version = info().version,
            },
        };
    }
    [[nodiscard]] virtual std::vector<OpticsProfileCandidate> profile_candidates(
        const AssetMetadata& metadata
    ) const {
        (void)metadata;
        return {};
    }
};

// Creates the open Lensfun adapter.  Supplying a directory lets tests and portable builds ship a
// pinned Lensfun database; omitting it delegates discovery to Lensfun's platform convention.  A
// build without Lensfun still returns a provider, but its only result is provider_unavailable.
[[nodiscard]] std::shared_ptr<const OpticsProvider> make_lensfun_optics_provider(
    std::optional<std::filesystem::path> database_directory = std::nullopt
);

} // namespace shadow::image
