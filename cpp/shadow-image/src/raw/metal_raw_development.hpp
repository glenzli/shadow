#pragma once

#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_denoise.hpp>

#include <optional>
#include <string>

namespace shadow::image::detail {

struct MetalRawDevelopmentAttempt final {
    std::optional<FusedRawFrameDevelopment> development;
    std::string diagnostic;
};

// This is an internal execution attempt rather than a public second RAW-denoise API. The
// semantic decision and receipt continue to belong to raw_denoise.cpp; Metal owns only an
// equivalent same-CFA implementation and can decline without changing that public contract.
struct MetalRawDenoiseAttempt final {
    bool applied = false;
    std::string diagnostic;
};

// Availability is a runtime property: a macOS build may still run without a usable Metal device.
[[nodiscard]] bool metal_raw_development_available() noexcept;

[[nodiscard]] bool metal_raw_denoise_available() noexcept;

// Runs the already-resolved same-CFA bilateral stage on Metal. `mode` is never skipped here;
// callers keep the skip policy, fallback, and receipt construction in the common CPU-facing
// raw-denoise stage.
[[nodiscard]] MetalRawDenoiseAttempt try_denoise_bayer_raw_frame_metal(
    RawFrame& frame,
    RawBayerDenoiseMode mode,
    double iso_sensitivity
);

// Metal Bayer reconstruction, CFA-aware area previews, camera transform and orientation into the
// common linear-sRGB u16 boundary. A typed unavailable result preserves CPU fallback semantics.
[[nodiscard]] MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_u16_metal(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    std::optional<std::uint32_t> preview_max_edge
);

} // namespace shadow::image::detail
