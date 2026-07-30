#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/decoder_types.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace shadow::image {
struct WarmEditPreviewGpuStats;
}

namespace shadow::image::detail {

struct WarmEditGpuPreparation;
class WarmEditGpuPresentationSurface;

enum class WarmEditGpuOutputIntent : std::uint8_t {
    host_rgb8,
    metal_presentation_surface,
};

struct WarmEditGpuGeometryContext final {
    PhotoGeometryLayout layout;
    PhotoGeometry geometry;
    GeometryPixelRect source_tile_rect;
    GeometryPixelRect output_rect;
    // Borrowed only for the synchronous render call. When present, the
    // geometry stage executes the fixed structural order Liquify -> Canvas.
    const PreparedPhotoLiquify* liquify = nullptr;
};

// One resident raster may represent either a complete warm proxy or a bounded full-detail
// working tile. Keep full-image coordinates explicit so deterministic finishing effects and
// display dithering do not acquire seams when the same kernels execute on independent tiles.
struct WarmEditGpuRenderContext final {
    AdjustmentExecutionContext adjustment;
    std::uint32_t display_origin_x = 0U;
    std::uint32_t display_origin_y = 0U;
    std::optional<WarmEditGpuGeometryContext> geometry;
    // Ordinary renderers and full-detail tiles keep the packed host contract. The opaque
    // interactive-frame route requests a frame-owned Metal surface and materializes RGB8 only
    // when an explicit non-Metal consumer asks for it.
    WarmEditGpuOutputIntent output_intent = WarmEditGpuOutputIntent::host_rgb8;
};

// Borrowed only for the duration of preparation. The successful session retains the same-device
// private fp32 buffer before this call returns; no host upload or fp32 materialization occurs.
// `external_resident_bytes` accounts for the RAW/CFA and other cache resources that remain live
// beside this session, while `resident_allowance_bytes` is the one combined device budget.
struct WarmEditGpuAdoptedSource final {
    Dimensions dimensions;
    std::size_t row_stride_bytes = 0U;
    void* native_device_handle = nullptr;
    void* native_buffer_handle = nullptr;
    std::uint64_t source_buffer_bytes = 0U;
    std::uint64_t external_resident_bytes = 0U;
    std::uint64_t resident_allowance_bytes = 0U;
    WorkingRgbSpace working_space;
    double level_zero_to_raster_scale_x = 1.0;
    double level_zero_to_raster_scale_y = 1.0;
};

// The resident backend owns one immutable source upload and two independently synchronized output
// slots. One owner may be a complete WarmEditPreviewSession or one bounded full-detail working
// tile. Process-global state is limited to immutable Metal device/pipeline objects, so unrelated
// photos never serialize on a global execution lock.
class WarmEditGpuSession final {
  public:
    WarmEditGpuSession(const WarmEditGpuSession&) = delete;
    WarmEditGpuSession& operator=(const WarmEditGpuSession&) = delete;
    ~WarmEditGpuSession();

    struct MaskCoverageResult final {
        std::uint32_t layer_index = 0U;
        Dimensions dimensions;
        std::uint32_t row_stride_bytes = 0U;
        std::vector<std::uint8_t> samples;
    };

    struct RenderResult final {
        Dimensions dimensions;
        std::vector<std::uint8_t> rgb8;
        std::shared_ptr<const WarmEditGpuPresentationSurface> presentation_surface;
        // Non-empty only when a requested native surface failed open to the exact existing host
        // RGB8 result. The effective pixels remain valid; callers can surface telemetry without
        // treating the whole Metal edit transaction as failed.
        std::string presentation_fallback_diagnostic;
        // Settled previews retain the pre-display scene-linear result for the exact existing
        // host analysis contract. Interactive previews leave this empty and avoid a large
        // device-to-host float copy.
        std::optional<FloatRgbImage> analyzed_linear;
        // Optional R8 selection evidence captured on the device from one layer's
        // pre-adjustment input and geometrically paired with this RGB frame.
        std::optional<MaskCoverageResult> mask_coverage;
        bool had_active_adjustments = false;
    };

    enum class RenderStatus : std::uint8_t {
        completed,
        cancelled,
        unavailable_or_failed,
    };

    struct RenderAttempt final {
        RenderStatus status = RenderStatus::unavailable_or_failed;
        std::optional<RenderResult> output;
        std::string diagnostic;
    };

    [[nodiscard]] RenderAttempt render(
        std::span<const AdjustmentNode> nodes,
        const EditExecutionPlan& plan,
        bool retain_linear_for_analysis,
        std::stop_token cancellation = {}
    ) const;
    [[nodiscard]] RenderAttempt render_layers(
        std::span<const AdjustmentLayer> layers,
        bool retain_linear_for_analysis,
        std::stop_token cancellation = {}
    ) const;
    [[nodiscard]] RenderAttempt render_layers(
        std::span<const AdjustmentLayer> layers,
        bool retain_linear_for_analysis,
        WarmEditGpuRenderContext context,
        std::stop_token cancellation = {}
    ) const;
    [[nodiscard]] RenderAttempt render_layers(
        std::span<const AdjustmentLayer> layers,
        bool retain_linear_for_analysis,
        WarmEditGpuRenderContext context,
        std::optional<std::uint32_t> target_layer_index,
        std::stop_token cancellation = {}
    ) const;
    [[nodiscard]] RenderAttempt render(
        std::span<const AdjustmentNode> nodes,
        const EditExecutionPlan& plan,
        bool retain_linear_for_analysis,
        WarmEditGpuRenderContext context,
        std::stop_token cancellation = {}
    ) const;

    [[nodiscard]] WarmEditPreviewGpuStats stats() const noexcept;

  private:
    struct Impl;
    explicit WarmEditGpuSession(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend struct WarmEditGpuPreparation;
    friend WarmEditGpuPreparation prepare_warm_edit_gpu_session(const FloatRgbImage& source);
    friend WarmEditGpuPreparation
    prepare_warm_edit_gpu_session(const WarmEditGpuAdoptedSource& source);
};

struct WarmEditGpuPreparation final {
    std::shared_ptr<WarmEditGpuSession> session;
    std::string diagnostic;
};

[[nodiscard]] WarmEditGpuPreparation prepare_warm_edit_gpu_session(const FloatRgbImage& source);
[[nodiscard]] WarmEditGpuPreparation
prepare_warm_edit_gpu_session(const WarmEditGpuAdoptedSource& source);

} // namespace shadow::image::detail
