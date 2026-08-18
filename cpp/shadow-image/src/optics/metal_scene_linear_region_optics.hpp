#pragma once

#include "lensfun_modifier_plan.hpp"

#include <shadow/image/fused_raw_development.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace shadow::image::raw_pipeline_detail {
class ResidentRawSource;
}

namespace shadow::image::detail {

class MetalRawPreviewResidentOutput;

inline constexpr std::string_view metal_scene_linear_region_optics_executor_identity =
    "shadow-metal-scene-linear-region-optics-v1";

class PreparedSceneLinearRegionOptics;

struct MetalSceneLinearRegionOpticsTelemetry final {
    std::uint64_t source_reupload_count = 0U;
    std::uint64_t source_fp32_readback_count = 0U;
    std::uint64_t coordinate_upload_count = 0U;
    std::uint64_t coordinate_upload_bytes = 0U;
    std::uint64_t profile_gain_upload_count = 0U;
    std::uint64_t profile_gain_upload_bytes = 0U;
    std::uint64_t optics_dispatch_count = 0U;
    std::uint64_t debug_readback_count = 0U;
    std::uint64_t debug_readback_bytes = 0U;
    std::uint64_t source_completion_fence_value = 0U;
    std::uint64_t completed_fence_value = 0U;
    bool source_slot_pinned_through_completion = false;
    bool invalidated = false;
};

class MetalSceneLinearRegionDeviceAccess;
class MetalSceneLinearRegionWarmPreviewAccess;

// The completed result owns one same-device private fp32 RGB buffer. It is move-only so C-d can
// retain one unambiguous resource owner across asynchronous display/edit consumers. Ordinary C++
// sees only immutable evidence; native handles remain private to the Objective-C++ access owner.
class MetalSceneLinearRegionLease final {
  public:
    MetalSceneLinearRegionLease(const MetalSceneLinearRegionLease&) = delete;
    MetalSceneLinearRegionLease& operator=(const MetalSceneLinearRegionLease&) = delete;
    MetalSceneLinearRegionLease(MetalSceneLinearRegionLease&&) noexcept;
    MetalSceneLinearRegionLease& operator=(MetalSceneLinearRegionLease&&) noexcept;
    ~MetalSceneLinearRegionLease();

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint64_t device_identity() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] std::uint64_t completion_fence_value() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] MetalSceneLinearRegionOpticsTelemetry telemetry() const noexcept;

    // This is an explicit numeric-oracle escape hatch, never the nominal product path.
    [[nodiscard]] SceneLinearRgbFrame debug_readback() const;

  private:
    struct Impl;
    explicit MetalSceneLinearRegionLease(std::unique_ptr<Impl> implementation) noexcept;

    [[nodiscard]] void* native_buffer_handle() const noexcept;
    [[nodiscard]] void* native_device_handle() const noexcept;
    [[nodiscard]] void* native_queue_handle() const noexcept;

    std::unique_ptr<Impl> implementation_;

    friend class MetalSceneLinearRegionDeviceAccess;
    friend class MetalSceneLinearRegionWarmPreviewAccess;
    friend MetalSceneLinearRegionLease develop_metal_scene_linear_region_optics(
        const raw_pipeline_detail::ResidentRawSource& source,
        lensfun_modifier_plan::PreparedRegion region,
        std::uint64_t source_resident_allowance_bytes
    );
    friend MetalSceneLinearRegionLease apply_metal_scene_linear_preview_optics(
        const MetalRawPreviewResidentOutput& source,
        const PreparedSceneLinearRegionOptics& optics,
        lensfun_modifier_plan::PreparedRegion region
    );
};

// `region` is immutable owner evidence from C-a. The aggregate source proves that the C-a optics
// plan and C-b RAW session belong to one publication. A present preimage is acquired internally
// from that source under the caller's remaining transaction allowance. All-out-of-bounds remaps
// carry no preimage and produce black before the final manual output vignette. Any admitted device
// failure invalidates the RAW source; invalid caller evidence is rejected before admission and
// leaves it healthy.
[[nodiscard]] MetalSceneLinearRegionLease develop_metal_scene_linear_region_optics(
    const raw_pipeline_detail::ResidentRawSource& source,
    lensfun_modifier_plan::PreparedRegion region,
    std::uint64_t source_resident_allowance_bytes
);

// Continues a bounded ordinary RAW preview from its already reconstructed fp32 Metal buffer.
// The caller must provide a full-preview region from the exact prepared optics plan. This avoids
// host materialization during a white-balance-only rebind while retaining the same Lensfun plan
// and source-rendering order as the materialized path.
[[nodiscard]] MetalSceneLinearRegionLease apply_metal_scene_linear_preview_optics(
    const MetalRawPreviewResidentOutput& source,
    const PreparedSceneLinearRegionOptics& optics,
    lensfun_modifier_plan::PreparedRegion region
);

[[nodiscard]] bool metal_scene_linear_region_optics_available() noexcept;
[[nodiscard]] const std::string& metal_scene_linear_region_optics_diagnostic() noexcept;

} // namespace shadow::image::detail
