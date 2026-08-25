#pragma once

#include <filesystem>

namespace shadow::image {

struct RawFrame;
struct RawFrameLinearTransform;

namespace probe_detail {

// Writes an offline comparison between Shadow's historical common-white CFA domain and the
// production scene-referred domain that retains per-CFA white-balance headroom. This owner is
// diagnostic-only: it does not participate in RAW development, cache identity, or desktop pixels.
void render_highlight_cfa_domain_diagnostic(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::filesystem::path& output_directory
);

// Offline threshold-domain ablation. Both branches share the exact decoded RawFrame, white
// balance, compiled opposed chrominance offsets, area sampler, and camera matrix; only candidate
// admission changes. It is deliberately unreachable from product rendering paths.
void render_highlight_threshold_ablation(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::filesystem::path& output_directory
);

} // namespace probe_detail

} // namespace shadow::image
