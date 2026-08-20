#pragma once

#include "bayer_sampling.hpp"

#include <shadow/image/fused_raw_development.hpp>

namespace shadow::image::detail {

// Shadow's source-domain highlight policy is deliberately H=0-like. The calibrated 2x2 CFA
// footprint only admits a continuous soft white ceiling; it never estimates missing colour or
// pulls a sample toward a locally inferred neutral. One-site coloured lights stay measured. As
// two or more physical sites run out of headroom, already-white-balanced components are smoothly
// limited to the canonical white point. A small residual scene-neutral stabilization is admitted
// only after that multi-site ceiling, without inventing spatial detail or replacing measured hue.
// The policy is shared by CPU and Metal. The disabled intent remains a strict measured-source
// reference for diagnosis and export comparison.
[[nodiscard]] CameraRgbSample reconstruct_cfa_highlights(
    CameraRgbSample camera,
    const RawFrameLinearTransform& transform,
    RawHighlightRecoveryIntent recovery
) noexcept;

} // namespace shadow::image::detail
