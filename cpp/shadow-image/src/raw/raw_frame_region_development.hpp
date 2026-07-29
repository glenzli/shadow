#pragma once

#include "bayer_sampling.hpp"
#include "raw_denoise_plan.hpp"

#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/photo_geometry.hpp>

#include <array>
#include <cstdint>

namespace shadow::image::raw_pipeline_detail {

// A region request names every coordinate space that participates in resident RAW execution.
// The requested core and optics preimage use oriented level-zero output coordinates. The
// reconstruction core uses un-oriented active-sensor coordinates, while the demosaic and denoise
// preimages use stored-sensor coordinates (including active margins).
class PreparedRawFrameRegion final {
  public:
    PreparedRawFrameRegion(const PreparedRawFrameRegion&) = default;
    PreparedRawFrameRegion& operator=(const PreparedRawFrameRegion&) = default;
    PreparedRawFrameRegion(PreparedRawFrameRegion&&) noexcept = default;
    PreparedRawFrameRegion& operator=(PreparedRawFrameRegion&&) noexcept = default;
    ~PreparedRawFrameRegion() = default;

    [[nodiscard]] GeometryPixelRect requested_core() const noexcept;
    [[nodiscard]] GeometryPixelRect reconstruction_core() const noexcept;
    [[nodiscard]] GeometryPixelRect demosaic_sensor_preimage() const noexcept;
    [[nodiscard]] GeometryPixelRect denoise_sensor_preimage() const noexcept;
    [[nodiscard]] GeometryPixelRect optics_output_preimage() const noexcept;
    [[nodiscard]] std::uint32_t demosaic_halo() const noexcept;
    [[nodiscard]] std::uint32_t denoise_halo() const noexcept;
    [[nodiscard]] RawDemosaicAlgorithm algorithm() const noexcept;

    [[nodiscard]] bool
    valid(const RawFrameDescriptor& descriptor, Dimensions output_dimensions) const noexcept;

  private:
    PreparedRawFrameRegion(
        GeometryPixelRect requested_core,
        GeometryPixelRect reconstruction_core,
        GeometryPixelRect demosaic_sensor_preimage,
        GeometryPixelRect denoise_sensor_preimage,
        GeometryPixelRect optics_output_preimage,
        std::uint32_t demosaic_halo,
        std::uint32_t denoise_halo,
        RawDemosaicAlgorithm algorithm,
        Dimensions source_storage_dimensions,
        Dimensions source_active_dimensions,
        Margins source_active_margins,
        std::int32_t source_orientation,
        std::uint32_t source_schema_version,
        RawFrameCfaLayout source_cfa_layout,
        std::array<RawCfaColor, 4U> source_bayer_2x2,
        RawDevelopmentQuality quality,
        RawBayerDenoiseMode denoise_mode
    ) noexcept;

    GeometryPixelRect requested_core_;
    GeometryPixelRect reconstruction_core_;
    GeometryPixelRect demosaic_sensor_preimage_;
    GeometryPixelRect denoise_sensor_preimage_;
    GeometryPixelRect optics_output_preimage_;
    std::uint32_t demosaic_halo_ = 0U;
    std::uint32_t denoise_halo_ = 0U;
    RawDemosaicAlgorithm algorithm_ = RawDemosaicAlgorithm::bayer_bilinear_v1;
    Dimensions source_storage_dimensions_;
    Dimensions source_active_dimensions_;
    Margins source_active_margins_;
    std::int32_t source_orientation_ = 0;
    std::uint32_t source_schema_version_ = 0U;
    RawFrameCfaLayout source_cfa_layout_ = RawFrameCfaLayout::unknown;
    std::array<RawCfaColor, 4U> source_bayer_2x2_{};
    RawDevelopmentQuality quality_ = RawDevelopmentQuality::balanced;
    RawBayerDenoiseMode denoise_mode_ = RawBayerDenoiseMode::skipped;

    friend PreparedRawFrameRegion prepare_raw_frame_region(
        const RawFrame& frame,
        RawDevelopmentQuality quality,
        const detail::PreparedRawBayerDenoise& denoise,
        GeometryPixelRect requested_core
    );
};

[[nodiscard]] Dimensions
oriented_raw_dimensions(Dimensions reconstruction_dimensions, std::int32_t orientation) noexcept;

[[nodiscard]] PreparedRawFrameRegion prepare_raw_frame_region(
    const RawFrame& frame,
    RawDevelopmentQuality quality,
    const detail::PreparedRawBayerDenoise& denoise,
    GeometryPixelRect requested_core
);

[[nodiscard]] SceneLinearRgbFrame develop_raw_frame_region_cpu(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    RawHighlightRecoveryIntent highlight_recovery,
    const PreparedRawFrameRegion& region
);

[[nodiscard]] RawDemosaicReceipt
raw_frame_region_demosaic_receipt(const RawFrame& frame, RawDevelopmentQuality quality) noexcept;

// Area previews share the exact camera-transform and sensor-highlight terminal with native
// resident regions. Keeping this one numeric owner prevents a future region optimization from
// drifting from the existing full-frame developer.
void write_raw_frame_transformed_pixel(
    const detail::CameraRgbSample& camera,
    const RawFrameLinearTransform& transform,
    bool neutralize_clipped_highlights,
    float* destination
) noexcept;

} // namespace shadow::image::raw_pipeline_detail
