#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace shadow::image::detail {
class WarmEditGpuSession;
class WarmEditGpuPresentationSurface;
enum class WarmEditGpuOutputIntent : std::uint8_t;
} // namespace shadow::image::detail

namespace shadow::image::edit_preview_detail {

struct PreparedEditPreviewPixels final {
    Dimensions dimensions;
    std::optional<FloatRgbImage> edited;
    std::vector<std::uint8_t> rgb;
    std::shared_ptr<const detail::WarmEditGpuPresentationSurface> presentation_surface;
    std::string presentation_fallback_diagnostic;
    EditPreviewExecutionReceipt execution;
    std::optional<EditPreviewMaskCoverage> mask_coverage;
};

[[nodiscard]] std::optional<PreparedEditPreviewPixels> prepare_edit_preview_pixels(
    const FloatRgbImage& working_proxy,
    const std::shared_ptr<detail::WarmEditGpuSession>& warm_gpu_session,
    std::string_view warm_gpu_diagnostic,
    std::span<const AdjustmentNode> nodes,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify,
    const SensorClippingMask* sensor_clipping_mask,
    const HighlightChromaRiskMap* highlight_chroma_risk_map,
    bool retain_linear_for_analysis,
    std::stop_token cancellation,
    detail::WarmEditGpuOutputIntent output_intent
);

[[nodiscard]] std::optional<PreparedEditPreviewPixels> prepare_edit_preview_layer_pixels(
    const FloatRgbImage& working_proxy,
    const std::shared_ptr<detail::WarmEditGpuSession>& warm_gpu_session,
    std::string_view warm_gpu_diagnostic,
    std::span<const AdjustmentLayer> layers,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify,
    const SensorClippingMask* sensor_clipping_mask,
    const HighlightChromaRiskMap* highlight_chroma_risk_map,
    bool retain_linear_for_analysis,
    std::stop_token cancellation,
    std::optional<std::uint32_t> target_layer_index,
    detail::WarmEditGpuOutputIntent output_intent,
    std::optional<std::uint32_t> target_component_index = std::nullopt
);

[[nodiscard]] std::optional<EditPreviewAnalysis> analyze_edit_preview(
    const FloatRgbImage& edited,
    const std::vector<std::uint8_t>& rgb,
    std::stop_token cancellation
);

} // namespace shadow::image::edit_preview_detail
