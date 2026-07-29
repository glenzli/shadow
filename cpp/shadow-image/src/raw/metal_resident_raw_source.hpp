#pragma once

#include "raw_frame_development_plan.hpp"

#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/raw_denoise.hpp>
#include <shadow/image/raw_frame.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace shadow::image::detail {
class MetalSceneLinearRegionOpticsAccess;
class PreparedSceneLinearRegionOptics;
} // namespace shadow::image::detail

namespace shadow::image::raw_pipeline_detail {

class PreparedRawFrameSource;
class ResidentRawSource;
struct ResidentRawSourceAttempt;

struct MetalResidentRawSourceTelemetry final {
    std::uint64_t source_upload_count = 0U;
    std::uint64_t source_upload_bytes = 0U;
    std::uint64_t denoise_dispatch_count = 0U;
    std::uint64_t region_dispatch_count = 0U;
    std::uint64_t region_readback_count = 0U;
    std::uint64_t region_readback_bytes = 0U;
    std::uint64_t full_frame_readback_count = 0U;
    std::uint64_t full_frame_readback_bytes = 0U;
    std::uint64_t source_buffer_release_count = 0U;
    std::uint64_t region_lease_count = 0U;
    std::uint64_t region_lease_release_count = 0U;
    std::uint64_t active_region_lease_count = 0U;
    std::uint64_t maximum_active_region_lease_count = 0U;
    std::uint64_t invalidation_count = 0U;
    std::uint64_t completed_fence_value = 0U;
    std::uint64_t retained_device_bytes = 0U;
    bool published = false;
    bool source_buffer_released = false;
    bool invalidated = false;
};

struct DevelopedMetalResidentRawRegion final {
    GeometryPixelRect requested_core;
    SceneLinearRgbFrame scene_linear;
};

// A successful lease pins its source session and one bounded device slot until destruction. The
// public surface deliberately exposes only immutable resource evidence; the native buffer and
// slot identity remain private to same-device Objective-C++ consumers.
class MetalResidentRawRegionLease final {
  public:
    MetalResidentRawRegionLease(const MetalResidentRawRegionLease&) = delete;
    MetalResidentRawRegionLease& operator=(const MetalResidentRawRegionLease&) = delete;
    MetalResidentRawRegionLease(MetalResidentRawRegionLease&&) noexcept;
    MetalResidentRawRegionLease& operator=(MetalResidentRawRegionLease&&) noexcept;
    ~MetalResidentRawRegionLease();

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint64_t device_identity() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] std::uint64_t completion_fence_value() const noexcept;

    // Compatibility materialization is explicit and counted. Nominal device consumers must move
    // the lease into a same-device executor instead of calling this method.
    [[nodiscard]] DevelopedMetalResidentRawRegion readback_compatibility() const;

  private:
    struct Impl;
    explicit MetalResidentRawRegionLease(std::unique_ptr<Impl> implementation) noexcept;

    [[nodiscard]] void* native_buffer_handle() const noexcept;
    [[nodiscard]] void* native_device_handle() const noexcept;
    [[nodiscard]] GeometryPixelRect requested_core() const noexcept;
    [[nodiscard]] Dimensions full_dimensions() const noexcept;
    void invalidate_source() const noexcept;

    std::unique_ptr<Impl> implementation_;

    friend class MetalResidentRawSource;
    friend class shadow::image::detail::MetalSceneLinearRegionOpticsAccess;
};

// One published session owns the provider-neutral, denoised Bayer plane and its independent
// command queue/output slots. It never owns a decoder provider, source filename, or camera-brand
// policy. A region failure after publication invalidates the session instead of silently changing
// backend provenance.
class MetalResidentRawSource final {
  public:
    MetalResidentRawSource(const MetalResidentRawSource&) = delete;
    MetalResidentRawSource& operator=(const MetalResidentRawSource&) = delete;
    MetalResidentRawSource(MetalResidentRawSource&&) noexcept;
    MetalResidentRawSource& operator=(MetalResidentRawSource&&) noexcept;
    ~MetalResidentRawSource();

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] const RawBayerDenoiseReceipt& raw_denoise_receipt() const noexcept;
    [[nodiscard]] RawDemosaicReceipt demosaic_receipt() const noexcept;
    [[nodiscard]] bool dcp_applied() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] MetalResidentRawSourceTelemetry telemetry() const noexcept;

    [[nodiscard]] MetalResidentRawRegionLease
    develop_device_region(GeometryPixelRect requested_core) const;

    // The bounded form admits slot growth only while the source's actual retained allocations
    // remain within the caller's transaction allowance. It is used by the full-detail aggregate
    // after reserving bytes for optics evidence and the destination working buffer.
    [[nodiscard]] MetalResidentRawRegionLease develop_device_region(
        GeometryPixelRect requested_core,
        std::uint64_t source_resident_allowance_bytes
    ) const;

    [[nodiscard]] DevelopedMetalResidentRawRegion
    develop_region(GeometryPixelRect requested_core) const;

  private:
    struct Impl;
    explicit MetalResidentRawSource(std::shared_ptr<Impl> implementation) noexcept;
    void invalidate() const noexcept;

    std::shared_ptr<Impl> implementation_;

    friend class MetalResidentRawRegionLease;
    friend class ResidentRawSource;
    friend ResidentRawSourceAttempt try_prepare_metal_resident_raw_source(
        PreparedRawFrameSource prepared,
        detail::PreparedSceneLinearRegionOptics optics
    );
};

[[nodiscard]] bool metal_resident_raw_source_available() noexcept;

} // namespace shadow::image::raw_pipeline_detail
