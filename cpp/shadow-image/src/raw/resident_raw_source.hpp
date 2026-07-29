#pragma once

#include "raw_frame_region_development.hpp"
#include "raw_frame_source_preparation.hpp"

#include "../optics/scene_linear_region_optics.hpp"

#include <shadow/image/raw_development_receipt.hpp>
#include <shadow/image/raw_pipeline.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace shadow::image::raw_pipeline_detail {

class MetalResidentRawSource;

struct DevelopedResidentRawRegion final {
    PreparedRawFrameRegion dependency_plan;
    SceneLinearRgbFrame scene_linear;
};

// Immutable aggregate root for CPU resident detail development. It owns one denoised CFA frame,
// the source-bound development plan/DCP, finalized provenance, and an independently executable
// pointwise optics plan. Repeated regions allocate only their requested RGB working raster.
class ResidentRawSource final {
  public:
    ResidentRawSource(const ResidentRawSource&) = delete;
    ResidentRawSource& operator=(const ResidentRawSource&) = delete;
    ResidentRawSource(ResidentRawSource&& other) noexcept;
    ResidentRawSource& operator=(ResidentRawSource&&) noexcept = delete;
    ~ResidentRawSource();

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] const RawDevelopmentReceipt& raw_development_receipt() const noexcept;
    [[nodiscard]] const RawPipelineReceipt& raw_pipeline_receipt() const noexcept;
    [[nodiscard]] const OpticsProfileReceipt& optics_receipt() const noexcept;
    [[nodiscard]] bool metal_resident() const noexcept;
    [[nodiscard]] bool device_path_valid() const noexcept;
    [[nodiscard]] const MetalResidentRawSource& metal_source() const;
    [[nodiscard]] const detail::PreparedSceneLinearRegionOptics& region_optics() const noexcept;
    void invalidate_device_path() const noexcept;

    [[nodiscard]] PreparedRawFrameRegion prepare_region(GeometryPixelRect requested_core) const;
    [[nodiscard]] DevelopedResidentRawRegion develop_region(GeometryPixelRect requested_core) const;

  private:
    ResidentRawSource(
        RawFrame frame,
        PreparedRawFrameDevelopment development,
        detail::PreparedSceneLinearRegionOptics optics,
        RawDevelopmentReceipt raw_development_receipt,
        RawPipelineReceipt raw_pipeline_receipt,
        std::uint64_t retained_bytes
    );
    ResidentRawSource(
        std::unique_ptr<MetalResidentRawSource> metal_source,
        PreparedRawFrameDevelopment development,
        detail::PreparedSceneLinearRegionOptics optics,
        RawDevelopmentReceipt raw_development_receipt,
        RawPipelineReceipt raw_pipeline_receipt
    );

    RawFrame frame_;
    PreparedRawFrameDevelopment development_;
    detail::PreparedSceneLinearRegionOptics optics_;
    RawDevelopmentReceipt raw_development_receipt_;
    RawPipelineReceipt raw_pipeline_receipt_;
    std::uint64_t retained_bytes_ = 0U;
    std::unique_ptr<MetalResidentRawSource> metal_source_;
    mutable std::atomic<bool> device_path_invalidated_{false};

    friend ResidentRawSource prepare_resident_raw_source(
        PreparedRawFrameSource prepared,
        detail::PreparedSceneLinearRegionOptics optics
    );
    friend ResidentRawSourceAttempt try_prepare_metal_resident_raw_source(
        PreparedRawFrameSource prepared,
        detail::PreparedSceneLinearRegionOptics optics
    );
};

struct ResidentRawSourceAttempt final {
    std::unique_ptr<ResidentRawSource> source;
    std::optional<PreparedRawFrameSource> fallback_source;
    std::string diagnostic;

    [[nodiscard]] bool published() const noexcept {
        return source != nullptr;
    }
};

[[nodiscard]] bool cpu_resident_raw_source_supported(
    const PreparedRawFrameSource& prepared,
    const detail::PreparedSceneLinearRegionOptics& optics
) noexcept;

[[nodiscard]] ResidentRawSource prepare_resident_raw_source(
    PreparedRawFrameSource prepared,
    detail::PreparedSceneLinearRegionOptics optics
);

[[nodiscard]] ResidentRawSourceAttempt try_prepare_metal_resident_raw_source(
    PreparedRawFrameSource prepared,
    detail::PreparedSceneLinearRegionOptics optics
);

} // namespace shadow::image::raw_pipeline_detail
