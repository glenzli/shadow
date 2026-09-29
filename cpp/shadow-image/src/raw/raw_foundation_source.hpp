#pragma once

#include <shadow/image/raw_foundation.hpp>
#include <shadow/image/raw_pipeline.hpp>

#include "raw_frame_source_preparation.hpp"

#include <string_view>

namespace shadow::image::raw_pipeline_detail {

[[nodiscard]] RawDevelopmentReceipt finalize_raw_foundation_receipt(
    const PreparedRawFrameDevelopment& prepared,
    const RawDevelopmentPlan& requested_plan,
    RawDevelopmentPlanNegotiationStatus negotiation_status,
    Dimensions rendered_dimensions,
    bool bounded_preview,
    std::string_view foundation_cache_identity,
    DcpColorExecutionBackend dcp_execution_backend
);

[[nodiscard]] RawPipelineReceipt finalize_raw_foundation_pipeline_receipt(
    RawPipelineReceipt prepared,
    RawHighlightRecoveryIntent highlight_recovery,
    std::string_view foundation_cache_identity
);

// Consumes one source-bound RawFrame preparation with one verified, borrowed
// linear-camera-RGB foundation. The stage first creates the common owned AI
// camera-RGB source basis together with immutable sensor evidence, then
// develops that basis into the final scene-linear output. The source frame is
// retained only while preparing the basis; no second camera-RGB copy is made.
[[nodiscard]] DevelopedSourceReference materialize_prepared_raw_foundation_source(
    PreparedRawFrameSource prepared,
    const RawFoundationCameraRgbView& foundation,
    const RawDevelopmentPlan& requested_plan
);

} // namespace shadow::image::raw_pipeline_detail
