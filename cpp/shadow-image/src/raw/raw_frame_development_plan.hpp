#pragma once

#include "raw_denoise_plan.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_development_receipt.hpp>
#include <shadow/image/raw_frame.hpp>

#include <cstdint>
#include <optional>

namespace shadow::image::raw_pipeline_detail {

// Immutable, source-bound policy prepared before a RawFrame executor is selected. The plan owns
// the optional DCP so full-frame and future resident executors cannot retain a borrowed profile or
// independently reinterpret source calibration, denoise intent, preview geometry, or provenance.
class PreparedRawFrameDevelopment final {
  public:
    PreparedRawFrameDevelopment(const PreparedRawFrameDevelopment&) = delete;
    PreparedRawFrameDevelopment& operator=(const PreparedRawFrameDevelopment&) = delete;
    PreparedRawFrameDevelopment(PreparedRawFrameDevelopment&&) noexcept = default;
    PreparedRawFrameDevelopment& operator=(PreparedRawFrameDevelopment&&) = delete;
    ~PreparedRawFrameDevelopment() = default;

    [[nodiscard]] const RawFrameDescriptor& descriptor() const noexcept;
    [[nodiscard]] const RawDevelopmentPlan& development_plan() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> preview_max_edge() const noexcept;
    [[nodiscard]] const RawFrameLinearTransform& linear_transform() const noexcept;
    [[nodiscard]] const DcpColorTransform* camera_profile() const noexcept;
    [[nodiscard]] const detail::PreparedRawBayerDenoise& raw_denoise() const noexcept;
    [[nodiscard]] RawDevelopmentBackendMode requested_backend() const noexcept;
    [[nodiscard]] Dimensions reconstruction_dimensions() const noexcept;
    [[nodiscard]] Dimensions diagnostic_dimensions() const noexcept;
    [[nodiscard]] double source_scene_luminance_percentile() const noexcept;
    // Creates a new immutable colour binding over the same already-prepared sensor stages.
    // Only the absolute camera white balance and its compiled colour transform may differ;
    // demosaic geometry, denoise policy, backend selection and preview bounds remain fixed.
    [[nodiscard]] PreparedRawFrameDevelopment rebind_color(
        RawDevelopmentPlan development_plan,
        RawFrameLinearTransform linear_transform,
        std::optional<DcpColorTransform> camera_profile,
        double source_scene_luminance_percentile
    ) const;

  private:
    PreparedRawFrameDevelopment(
        RawFrameDescriptor descriptor,
        RawDevelopmentPlan development_plan,
        std::optional<std::uint32_t> preview_max_edge,
        RawFrameLinearTransform linear_transform,
        std::optional<DcpColorTransform> camera_profile,
        detail::PreparedRawBayerDenoise raw_denoise,
        RawDevelopmentBackendMode requested_backend,
        Dimensions reconstruction_dimensions,
        Dimensions diagnostic_dimensions,
        double source_scene_luminance_percentile
    );

    friend PreparedRawFrameDevelopment prepare_raw_frame_development(
        const RawFrame& frame,
        RawDevelopmentPlan development_plan,
        std::optional<std::uint32_t> preview_max_edge,
        std::optional<DcpColorTransform> camera_profile,
        double iso_sensitivity
    );

    RawFrameDescriptor descriptor_;
    RawDevelopmentPlan development_plan_;
    std::optional<std::uint32_t> preview_max_edge_;
    RawFrameLinearTransform linear_transform_;
    std::optional<DcpColorTransform> camera_profile_;
    detail::PreparedRawBayerDenoise raw_denoise_;
    RawDevelopmentBackendMode requested_backend_ = RawDevelopmentBackendMode::automatic;
    Dimensions reconstruction_dimensions_;
    Dimensions diagnostic_dimensions_;
    double source_scene_luminance_percentile_ = 0.0;
};

[[nodiscard]] PreparedRawFrameDevelopment prepare_raw_frame_development(
    const RawFrame& frame,
    RawDevelopmentPlan development_plan,
    std::optional<std::uint32_t> preview_max_edge,
    std::optional<DcpColorTransform> camera_profile,
    double iso_sensitivity
);

// Compiles only the camera-space colour decision. This is intentionally separate from complete
// sensor-stage preparation so an interactive RAW preview can reuse its decoded/denoised
// foundation while temperature/tint recompiles white balance and DCP state.
[[nodiscard]] RawFrameLinearTransform prepare_raw_frame_linear_transform(
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance,
    const DcpColorTransform* camera_profile
);

// Execution contributes only facts that cannot be known during preparation. Keeping receipt
// finalization beside the prepared policy prevents full-frame and resident paths from drifting.
[[nodiscard]] RawDevelopmentReceipt finalize_raw_frame_development_receipt(
    const PreparedRawFrameDevelopment& prepared,
    Dimensions rendered_dimensions,
    const RawDemosaicReceipt& demosaic,
    RawDevelopmentBackend backend,
    const RawBayerDenoiseReceipt& raw_denoise,
    DcpColorExecutionBackend dcp_execution_backend
);

} // namespace shadow::image::raw_pipeline_detail
