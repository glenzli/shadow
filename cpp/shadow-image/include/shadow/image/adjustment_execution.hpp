#pragma once

#include <shadow/image/edit.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace shadow::image {

// Provenance-bearing dispatcher for one complete adjustment stage. The CPU implementation is
// the authoritative oracle. Metal is intentionally all-or-nothing: a render either executes every
// active supported node on Metal in source order, or restarts the complete stage on CPU from the
// immutable input.
enum class AdjustmentBackend : std::uint8_t {
    cpu,
    metal,
};

enum class AdjustmentBackendMode : std::uint8_t {
    automatic,
    cpu,
    metal,
};

inline constexpr std::uint32_t adjustment_cpu_backend_version = 1U;
inline constexpr std::uint32_t adjustment_metal_backend_version = 1U;

[[nodiscard]] std::string_view adjustment_backend_identity(
    AdjustmentBackend backend
) noexcept;
[[nodiscard]] bool adjustment_backend_available(AdjustmentBackend backend) noexcept;

// Runtime developer/testing override shared with RAW development and display output:
//   SHADOW_IMAGE_ACCELERATION=auto|cpu|metal
[[nodiscard]] AdjustmentBackendMode adjustment_backend_mode_from_environment();

struct AdjustmentExecutionResult final {
    FloatRgbImage pixels;
    AdjustmentBackend backend = AdjustmentBackend::cpu;
    // Automatic selection records why Metal was unable to execute the complete stage. Forced
    // CPU/Metal never reports fallback. Diagnostics are runtime-only and never enter a durable
    // cache identity.
    bool fell_back = false;
    std::string diagnostic;

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] AdjustmentExecutionResult execute_adjustment_nodes_with_backend(
    const FloatRgbImage& input,
    std::span<const AdjustmentNode> nodes,
    AdjustmentExecutionContext context,
    AdjustmentBackendMode backend_mode
);

[[nodiscard]] AdjustmentExecutionResult execute_adjustment_nodes_accelerated(
    const FloatRgbImage& input,
    std::span<const AdjustmentNode> nodes,
    AdjustmentExecutionContext context = {}
);

} // namespace shadow::image
