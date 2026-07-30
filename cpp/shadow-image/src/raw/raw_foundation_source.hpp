#pragma once

#include <shadow/image/raw_foundation.hpp>
#include <shadow/image/raw_pipeline.hpp>

#include "raw_frame_source_preparation.hpp"

namespace shadow::image::raw_pipeline_detail {

// Consumes one source-bound RawFrame preparation with one verified, borrowed
// linear-camera-RGB foundation. The source frame remains available until
// clipping diagnostics are projected; the foundation pixels are copied only
// into the final owned scene-linear output.
[[nodiscard]] DevelopedSourceReference materialize_prepared_raw_foundation_source(
    PreparedRawFrameSource prepared,
    const RawFoundationCameraRgbView& foundation,
    const RawDevelopmentPlan& requested_plan
);

} // namespace shadow::image::raw_pipeline_detail
