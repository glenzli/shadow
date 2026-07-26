#pragma once

#include <shadow/image/decoder.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_development.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace shadow::image {

struct CameraProfileCatalog;

// The RAW pipeline is a host policy, not a camera-provider setting. Providers only expose
// source samples and capabilities; Shadow decides whether those samples enter its sensor-domain
// developer or the provider's processed-RGB compatibility path. Keeping the choice explicit
// makes fallbacks visible in cache identities and allows a photographer/developer to require one
// route while the implementation is still being validated.
inline constexpr std::uint32_t raw_pipeline_policy_schema_version = 1U;
inline constexpr std::uint32_t raw_pipeline_receipt_schema_version = 1U;
inline constexpr std::uint32_t shadow_raw_frame_developer_version = 1U;

enum class RawPipelineMode : std::uint8_t {
    automatic,
    require_shadow_raw_frame,
    require_provider_processed,
};

enum class RawPipelinePath : std::uint8_t {
    decoded_raster,
    shadow_raw_frame,
    provider_processed_compatibility,
};

enum class RawCameraProfileStatus : std::uint8_t {
    // Raster/provider-processed paths never inspect Shadow's local DCP catalog.
    not_considered,
    // RawFrame path inspected a concrete immutable catalog snapshot and found no exact camera.
    no_match,
    // One exact DCP was compiled and applied completely.
    applied,
    // One exact DCP matched but was not partially applied because it carried unsupported
    // rendering stages or invalid color math. The generic provider matrix remains in use.
    matched_not_applied,
};

struct RawPipelinePolicy final {
    std::uint32_t schema_version = raw_pipeline_policy_schema_version;
    RawPipelineMode mode = RawPipelineMode::automatic;

    auto operator<=>(const RawPipelinePolicy&) const = default;
};

struct RawPipelineReceipt final {
    std::uint32_t schema_version = raw_pipeline_receipt_schema_version;
    RawPipelinePath path = RawPipelinePath::decoded_raster;
    std::string pipeline_identity;
    std::string source_provider_id;
    std::string source_provider_version;
    std::string fallback_reason;
    std::uint32_t raw_frame_schema_version = 0U;
    std::uint32_t raw_developer_version = 0U;
    // A bounded, scene-linear 99th-percentile estimate measured from the owned sensor frame
    // before the preview/detail branch.  It makes Shadow Standard's content-normalization
    // stable when a fitted CFA preview and a full-resolution render are prepared separately.
    // Compatibility RGB/raster paths intentionally leave this empty: their providers own the
    // rendered source and the existing RGB measurement remains their only truthful fallback.
    std::optional<double> source_scene_luminance_percentile;
    RawDevelopmentPlan requested_plan;
    RawDevelopmentPlan effective_plan;
    RawCameraProfileStatus camera_profile_status = RawCameraProfileStatus::not_considered;
    std::string camera_profile_catalog_identity;
    std::string camera_profile_identity;
    std::string camera_profile_name;
    std::string camera_profile_diagnostic;
    std::uint32_t camera_profile_developer_version = 0U;

    [[nodiscard]] bool valid() const noexcept;
};

// Ordinary rendered files and compatibility providers stay at the packed u16 boundary. Shadow's
// owned RawFrame route instead carries the scene-linear fp32 result into the common edit graph so
// a transform above display white does not become unrecoverable before the first adjustment.
using DevelopedSourcePixels = std::variant<PixelBuffer, SceneLinearRgbFrame>;

struct DevelopedSourceReference final {
    DevelopedSourcePixels source;
    // Provenance is deliberately not coupled to either source container. A scene-linear frame is
    // not a provider `PixelBuffer`, while an optics provider may legitimately return a fresh
    // packed buffer without copying sidecars.
    RawDevelopmentReceipt raw_development_receipt;
    RawPipelineReceipt pipeline_receipt;
    // RAW-only source diagnostics are produced while the owned CFA frame is already resident.
    // They remain optional because raster and provider-processed compatibility routes have no
    // truthful sensor-domain data to report.
    std::optional<SensorClippingMask> sensor_clipping_mask;
};

[[nodiscard]] constexpr RawPipelinePolicy default_raw_pipeline_policy() noexcept {
    return {};
}

// These are the capabilities of Shadow's own sensor-domain developer, not a decoder's optional
// already-processed RGB renderer. Any source that supplies a valid owned RawFrame is developed
// against this contract after crossing the provider boundary.
[[nodiscard]] RawDevelopmentCapabilities shadow_raw_frame_development_capabilities() noexcept;
[[nodiscard]] RawDevelopmentPlanNegotiation negotiate_shadow_raw_frame_development_plan(
    const RawDevelopmentPlan& requested
) noexcept;

// Developer/testing override:
//   SHADOW_RAW_PIPELINE=auto|raw-frame|processed
// Unknown values fail closed when a source is prepared instead of silently changing appearance.
[[nodiscard]] RawPipelinePolicy raw_pipeline_policy_from_environment();

[[nodiscard]] std::string raw_pipeline_policy_identity(const RawPipelinePolicy& policy);
[[nodiscard]] std::string raw_pipeline_receipt_identity(const RawPipelineReceipt& receipt);

// Produces Shadow's common linear-sRGB source contract. Raster sources keep their decoded 16-bit
// appearance. RAW sources use the owned RawFrame developer when its calibration/layout/pending
// correction contract is currently supported; automatic mode records an explicit provider-RGB
// compatibility fallback otherwise.
//
// A preview edge asks the RawFrame developer to reconstruct only a bounded source raster. Detail
// and export callers pass std::nullopt and retain native oriented dimensions.
[[nodiscard]] DevelopedSourceReference develop_source_reference(
    const DecodeSession& session,
    const RawDevelopmentPlan& plan,
    std::optional<std::uint32_t> preview_max_edge = std::nullopt,
    const RawPipelinePolicy& policy = default_raw_pipeline_policy()
);

// Explicit immutable catalog overload used by profile-manager refreshes and deterministic tests.
// Production callers normally use the process-lifetime local snapshot above.
[[nodiscard]] DevelopedSourceReference develop_source_reference(
    const DecodeSession& session,
    const RawDevelopmentPlan& plan,
    std::optional<std::uint32_t> preview_max_edge,
    const RawPipelinePolicy& policy,
    const CameraProfileCatalog& camera_profiles
);

} // namespace shadow::image
