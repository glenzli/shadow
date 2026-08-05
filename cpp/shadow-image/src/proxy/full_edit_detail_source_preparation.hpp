#pragma once

#include "../raw/resident_raw_source.hpp"

#include <shadow/image/decoder_metadata.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/optics.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_development_receipt.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/source_rendering.hpp>

#include <cstdint>
#include <variant>

namespace shadow::image {
class DecodeSession;
}

namespace shadow::image::proxy_detail {

enum class FullDetailSourceStorage : std::uint8_t {
    materialized_scene_linear,
    resident_raw_candidate,
};

[[nodiscard]] bool full_detail_source_allows_metal_publication(
    const FullEditDetailSourceRequirements& requirements
) noexcept;

// Metadata admission follows the source representation that may actually be retained. Resident
// RAW candidates are bounded as uint16 CFA storage; any path that can materialize complete
// scene-linear RGB keeps the stricter fp32-RGB bound.
void validate_full_detail_source_preflight(
    const AssetMetadata& metadata,
    FullDetailSourceStorage storage
);

// One completed source-routing transaction. The variant encodes exactly one materialized or
// resident owner; no null/dual state can escape preparation. The public FullEditDetailSession
// consumes this value and remains the owner of tile composition, so provider/RawFrame negotiation
// does not leak back into the render lifecycle.
struct PreparedFullEditDetailSource final {
    std::variant<DevelopedSourcePixels, raw_pipeline_detail::ResidentRawSource> source;
    std::uint64_t retained_bytes = 0U;
    RawDevelopmentReceipt raw_development_receipt;
    RawPipelineReceipt raw_pipeline_receipt;
    OpticsProfileReceipt optics_receipt;
    SourceRenderingReceipt source_rendering;

    [[nodiscard]] bool resident() const noexcept {
        return std::holds_alternative<raw_pipeline_detail::ResidentRawSource>(source);
    }
};

// Owns source preflight, provider compatibility, one-decode PreparedRawFrameSource routing, and
// the forced-CPU versus automatic/forced-Metal publication boundary. Automatic failures before
// publication may materialize the returned intact owner; no failure after publication reaches
// this preparation boundary.
[[nodiscard]] PreparedFullEditDetailSource prepare_full_edit_detail_source(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
    const FullEditDetailSourceRequirements& requirements,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
);

// Crash-isolated ordinary RAW source. The staged frame owns sensor samples;
// the in-process session contributes metadata/profile context only.
[[nodiscard]] PreparedFullEditDetailSource prepare_full_edit_detail_source(
    const DecodeSession& metadata_session,
    RawFrame staged_frame,
    const RawDevelopmentPlan& raw_development_plan,
    const FullEditDetailSourceRequirements& requirements,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
);

// Foundation input is already reconstructed and therefore always publishes the materialized
// scene-linear variant. The overload remains separate from resident-CFA admission and never
// falls back to the original RawFrame route.
[[nodiscard]] PreparedFullEditDetailSource prepare_full_edit_detail_source(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    const FullEditDetailSourceRequirements& requirements,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
);

[[nodiscard]] PreparedFullEditDetailSource prepare_full_edit_detail_source(
    const DecodeSession& metadata_session,
    RawFrame staged_frame,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    const FullEditDetailSourceRequirements& requirements,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
);

} // namespace shadow::image::proxy_detail
