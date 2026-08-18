#pragma once

#include "warm_edit_gpu.hpp"

#include "../optics/metal_scene_linear_region_optics.hpp"
#include "../raw/metal_resident_raw_source.hpp"
#include "../raw/resident_raw_source.hpp"

#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace shadow::image::detail {

inline constexpr std::uint64_t maximum_full_edit_detail_metal_resident_bytes =
    512ULL * 1'024ULL * 1'024ULL;

// Applies the canonical source-rendering receipt in place to a same-device fp32 RGB buffer.
// Both bounded RAW preview rebinding and full-detail preparation use this owner so exposure and
// tone-curve math cannot drift merely because one source remains resident on Metal.
struct MetalSourceRenderingInPlaceAttempt final {
    bool applied = false;
    std::uint64_t curve_upload_bytes = 0U;
    std::string diagnostic;
};

[[nodiscard]] MetalSourceRenderingInPlaceAttempt apply_source_rendering_in_place_metal(
    void* native_device_handle,
    void* native_queue_handle,
    void* native_buffer_handle,
    Dimensions dimensions,
    const SourceRenderingReceipt& source_rendering
);

// Runtime-only evidence for one C-b -> C-c -> source-render -> warm-adoption transaction. These
// counters never enter a Recipe or durable cache identity.
struct FullEditDetailMetalSourceTelemetry final {
    raw_pipeline_detail::MetalResidentRawSourceTelemetry raw;
    MetalSceneLinearRegionOpticsTelemetry optics;
    WarmEditPreviewGpuStats warm;
    std::uint64_t source_rendering_dispatch_count = 0U;
    std::uint64_t source_rendering_curve_upload_count = 0U;
    std::uint64_t source_rendering_curve_upload_bytes = 0U;
    std::uint64_t warm_source_adoption_count = 0U;
    std::uint64_t combined_resident_bytes = 0U;
    std::uint64_t resident_allowance_bytes = 0U;
};

struct FullEditDetailMetalSourcePreparation final {
    std::shared_ptr<WarmEditGpuSession> session;
    FullEditDetailMetalSourceTelemetry telemetry;
    std::string diagnostic;

    [[nodiscard]] bool published() const noexcept {
        return session != nullptr;
    }
};

// Prepares one immutable source-rendered working region. The resident RAW source must already be
// terminally published, so any failure invalidates its device path and may not be converted into
// an automatic CPU tile fallback. `other_cached_resident_bytes` accounts for sibling warm-source
// entries that remain live in the same full-detail cache.
[[nodiscard]] FullEditDetailMetalSourcePreparation prepare_full_edit_detail_metal_source(
    raw_pipeline_detail::ResidentRawSource& source,
    const SourceRenderingReceipt& source_rendering,
    GeometryPixelRect working_rect,
    std::uint64_t other_cached_resident_bytes
);

[[nodiscard]] bool full_edit_detail_metal_source_available() noexcept;
[[nodiscard]] const std::string& full_edit_detail_metal_source_diagnostic() noexcept;

} // namespace shadow::image::detail
