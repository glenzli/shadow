#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/photo_geometry.hpp>
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
} // namespace shadow::image::detail

namespace shadow::image::edit_preview_detail {

struct PreparedEditPreviewPixels final {
    Dimensions dimensions;
    std::optional<FloatRgbImage> edited;
    std::vector<std::uint8_t> rgb;
    EditPreviewExecutionReceipt execution;
};

[[nodiscard]] std::optional<PreparedEditPreviewPixels> prepare_edit_preview_pixels(
    const FloatRgbImage& working_proxy,
    const std::shared_ptr<detail::WarmEditGpuSession>& warm_gpu_session,
    std::string_view warm_gpu_diagnostic,
    std::span<const AdjustmentNode> nodes,
    const PhotoGeometry& geometry,
    bool retain_linear_for_analysis,
    std::stop_token cancellation
);

[[nodiscard]] std::optional<PreparedEditPreviewPixels> prepare_edit_preview_layer_pixels(
    const FloatRgbImage& working_proxy,
    const std::shared_ptr<detail::WarmEditGpuSession>& warm_gpu_session,
    std::string_view warm_gpu_diagnostic,
    std::span<const AdjustmentLayer> layers,
    const PhotoGeometry& geometry,
    bool retain_linear_for_analysis,
    std::stop_token cancellation
);

[[nodiscard]] std::optional<EditPreviewAnalysis> analyze_edit_preview(
    const FloatRgbImage& edited,
    const std::vector<std::uint8_t>& rgb,
    std::stop_token cancellation
);

} // namespace shadow::image::edit_preview_detail
