#pragma once

#include <shadow/image/raw_pipeline.hpp>

#include "raw_frame_source_preparation.hpp"

#include <cstdint>
#include <string>

namespace shadow::image::raw_pipeline_detail {

struct DevelopedRawFrame final {
    DevelopedSourcePixels source;
    RawDevelopmentReceipt raw_development_receipt;
    SensorClippingMask sensor_clipping_mask;
    SensorHighlightChromaConfidence sensor_highlight_chroma_confidence;
    RawDevelopmentBackend backend = RawDevelopmentBackend::cpu;
    RawHighlightRecoveryIntent highlight_recovery = RawHighlightRecoveryIntent::provider_default;
    std::string raw_denoise_cache_identity;
    double source_scene_luminance_percentile = 0.0;
};

// Materializes the owner-controlled source pair. The executor receives the opaque aggregate
// rather than independent frame/plan arguments, so source-wide calibration cannot be re-paired.
[[nodiscard]] DevelopedRawFrame develop_raw_frame(PreparedRawFrameSource& prepared_source);

} // namespace shadow::image::raw_pipeline_detail
