#pragma once

#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_denoise.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_frame.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include "raw_denoise_plan.hpp"

#include <optional>
#include <string>

namespace shadow::image {

struct DcpColorTransform;

namespace detail {

// Optional continuations share the uploaded sensor plane or reconstructed tile but retain their
// own semantic owners. Grouping them here keeps the transaction API stable as resident stages are
// added without turning the reconstruction entry point into a list of unrelated pointer flags.
struct MetalRawDevelopmentContinuations final {
    const DcpColorTransform* dcp_color_transform = nullptr;
    const PreparedRawBayerDenoise* raw_denoise = nullptr;
    bool project_sensor_clipping = false;
};

struct MetalRawDevelopmentAttempt final {
    std::optional<FusedRawFrameDevelopment> development;
    std::optional<SensorClippingMask> sensor_clipping_mask;
    bool raw_denoise_applied = false;
    bool dcp_applied = false;
    std::string diagnostic;
};

// This is an internal execution attempt rather than a public second RAW-denoise API. The
// semantic decision and receipt continue to belong to raw_denoise.cpp; Metal owns only an
// equivalent same-CFA implementation and can decline without changing that public contract.
struct MetalRawDenoiseAttempt final {
    bool applied = false;
    std::string diagnostic;
};

// DCP input rendering remains a separate semantic stage after sensor reconstruction. Its compact
// encoder can either continue directly from each reconstructed Metal tile or execute against a
// standalone scene-linear frame without changing the public DCP transform contract.
struct MetalDcpColorDevelopmentAttempt final {
    bool applied = false;
    std::string diagnostic;
};

// Availability is a runtime property: a macOS build may still run without a usable Metal device.
[[nodiscard]] bool metal_raw_development_available() noexcept;

[[nodiscard]] bool metal_raw_denoise_available() noexcept;

[[nodiscard]] bool metal_dcp_color_development_available() noexcept;

// Runs the already-resolved same-CFA bilateral stage on Metal. `mode` is never skipped here;
// callers keep the skip policy, fallback, and receipt construction in the common CPU-facing
// raw-denoise stage.
[[nodiscard]] MetalRawDenoiseAttempt try_denoise_bayer_raw_frame_metal(
    RawFrame& frame,
    RawBayerDenoiseMode mode,
    double iso_sensitivity
);

// Metal Bayer reconstruction, CFA-aware area previews, camera transform and orientation into the
// common fp32 scene-linear sRGB boundary. A typed unavailable result preserves CPU fallback
// semantics.
[[nodiscard]] MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_f32_metal(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    std::optional<std::uint32_t> preview_max_edge,
    RawHighlightRecoveryIntent highlight_recovery,
    RawDevelopmentQuality quality,
    MetalRawDevelopmentContinuations continuations = {}
);

[[nodiscard]] MetalDcpColorDevelopmentAttempt try_apply_dcp_color_rendering_stages_metal(
    SceneLinearRgbFrame& pixels,
    const DcpColorTransform& transform
);

} // namespace detail
} // namespace shadow::image
