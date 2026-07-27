#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/optics.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_development_plan.hpp>

#include <span>

namespace shadow::image {

// Renders a standard display-referred JPEG while keeping adjustment math in explicitly linear
// sRGB working RGB. LibRaw is configured to provide processed linear-light u16; the versioned
// output boundary gamut-maps and applies the sRGB transfer only after node execution.
[[nodiscard]] EncodedProxy render_edited_reference_proxy_jpeg(
    const DecodeSession& session,
    std::span<const AdjustmentNode> nodes,
    ProxyRequest request = {},
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// Layer-aware equivalent of the standard reference proxy route. It is deliberately a separate
// entry point so the established flat-node call path keeps its accelerated implementation until
// a layer-aware GPU executor is available. The output contract remains the same JPEG proxy.
[[nodiscard]] EncodedProxy render_edited_reference_proxy_jpeg_layers(
    const DecodeSession& session,
    std::span<const AdjustmentLayer> layers,
    ProxyRequest request = {},
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// Explicit plan-bearing forms used by cache-aware callers. Existing overloads above select the
// canonical preview plan, preserving their source-compatible behavior.
[[nodiscard]] EncodedProxy render_edited_reference_proxy_jpeg(
    const DecodeSession& session,
    std::span<const AdjustmentNode> nodes,
    ProxyRequest request,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

[[nodiscard]] EncodedProxy render_edited_reference_proxy_jpeg_layers(
    const DecodeSession& session,
    std::span<const AdjustmentLayer> layers,
    ProxyRequest request,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

} // namespace shadow::image
