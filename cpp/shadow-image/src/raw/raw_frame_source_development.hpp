#pragma once

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/raw_pipeline.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace shadow::image::raw_pipeline_detail {

struct DevelopedRawFrame final {
    DevelopedSourcePixels source;
    RawDevelopmentReceipt raw_development_receipt;
    SensorClippingMask sensor_clipping_mask;
    RawDevelopmentBackend backend = RawDevelopmentBackend::cpu;
    RawHighlightRecoveryIntent highlight_recovery =
        RawHighlightRecoveryIntent::provider_default;
    std::string raw_denoise_cache_identity;
    double source_scene_luminance_percentile = 0.0;
};

[[nodiscard]] DevelopedRawFrame develop_raw_frame(
    RawFrame frame,
    const RawDevelopmentPlan& plan,
    std::optional<std::uint32_t> preview_max_edge,
    const DcpColorTransform* camera_profile,
    double iso_sensitivity
);

} // namespace shadow::image::raw_pipeline_detail
