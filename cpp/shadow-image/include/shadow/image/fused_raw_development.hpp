#pragma once

#include <shadow/image/raw_development.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_frame.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace shadow::image {

// The backend is part of RAW-development provenance. CPU and Metal deliberately share the same
// public pixel contract, but Metal performs its hot arithmetic in fp32 and is therefore not
// assumed to be bit-identical to the CPU reference.
enum class RawDevelopmentBackend : std::uint8_t {
    cpu,
    metal,
};

enum class RawDevelopmentBackendMode : std::uint8_t {
    automatic,
    cpu,
    metal,
};

inline constexpr std::uint32_t fused_raw_cpu_backend_version = 1U;
inline constexpr std::uint32_t fused_raw_metal_backend_version = 1U;

[[nodiscard]] std::string_view
raw_development_backend_identity(RawDevelopmentBackend backend) noexcept;
[[nodiscard]] bool raw_development_backend_available(RawDevelopmentBackend backend) noexcept;
[[nodiscard]] std::string_view
raw_highlight_treatment_identity(RawHighlightRecoveryIntent intent) noexcept;

// Runtime developer/testing override:
//   SHADOW_IMAGE_ACCELERATION=auto|cpu|metal
// `auto` always falls back to the CPU reference inside the same RAW path when Metal is
// unavailable or rejects an eligible request. `metal` fails explicitly for eligible full-detail
// and CFA-area-preview work.
[[nodiscard]] RawDevelopmentBackendMode raw_development_backend_mode_from_environment();

// Camera RGB -> linear sRGB/Rec.709 D65, row-major. The caller compiles all color decisions into
// this one immutable matrix before rendering:
//
// - the generic provider route folds AsShotNeutral white balance into its camera matrix;
// - the DCP route passes DcpColorTransform::camera_to_linear_srgb_d65, which already contains
//   white balance, chromatic adaptation, and BaselineExposureOffset.
//
// Keeping profile interpretation outside this hot loop lets the same bounded renderer serve
// public LibRaw and independently implemented provider paths without learning either profile
// format.
struct RawFrameLinearTransform final {
    std::array<double, 9U> camera_to_linear_srgb_d65{};
    // Camera-space response to the selected neutral, normalized to green.
    // Highlight recovery uses this before the matrix so clipped CFA channels
    // are reconstructed as measured camera ratios instead of painted after
    // colour conversion.
    std::array<double, 3U> camera_neutral{1.0, 1.0, 1.0};

    [[nodiscard]] bool valid() const noexcept;
};

// The fused renderer returns scene-linear fp32 working RGB and demosaic provenance. It never
// allocates a full-resolution float Camera-RGB image, but intentionally retains the transformed
// result above 1.0 so later highlight controls operate on measured RAW headroom.
struct FusedRawFrameDevelopment final {
    SceneLinearRgbFrame scene_linear;
    RawDemosaicReceipt demosaic_receipt;
    RawDevelopmentBackend backend = RawDevelopmentBackend::cpu;
    RawHighlightRecoveryIntent highlight_recovery = RawHighlightRecoveryIntent::provider_default;

    [[nodiscard]] bool valid() const noexcept;
};

// Reconstructs Bayer samples, applies the precompiled camera transform, maps the provider's
// orientation, and writes scene-linear fp32 samples in one bounded parallel row pass. `balanced`
// selects the fast bilinear detail baseline, while `high` selects the host-owned edge-aware
// contract on either the CPU reference or Metal executor for native-size detail and export.
// Bounded previews always retain CFA-area integration.
//
// A missing preview edge selects full-resolution 3x3 bilinear reconstruction. A non-zero preview
// edge selects the same CFA-aware sensor-footprint integration as demosaic_bayer_preview().
// Existing standalone demosaic functions remain the correctness/reference API.
[[nodiscard]] FusedRawFrameDevelopment develop_bayer_linear_srgb_f32_fused(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    std::optional<std::uint32_t> preview_max_edge = std::nullopt,
    RawHighlightRecoveryIntent highlight_recovery = RawHighlightRecoveryIntent::provider_default,
    RawDevelopmentQuality quality = RawDevelopmentQuality::balanced
);

// Deterministic selector used by parity tests and diagnostics. Production callers normally use
// the environment-aware overload above.
[[nodiscard]] FusedRawFrameDevelopment develop_bayer_linear_srgb_f32_fused_with_backend(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    std::optional<std::uint32_t> preview_max_edge,
    RawDevelopmentBackendMode backend_mode,
    RawHighlightRecoveryIntent highlight_recovery = RawHighlightRecoveryIntent::provider_default,
    RawDevelopmentQuality quality = RawDevelopmentQuality::balanced
);

} // namespace shadow::image
