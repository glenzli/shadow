#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/decoder_types.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/lut.hpp>
#include <shadow/image/optics.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_development_receipt.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/reference_pixels.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/working_rgb.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

// A square proxy at this limit occupies at most 192 MiB as interleaved RGB float32.
// Typical 3:2 photos at the UI's 1600/2048 edge use substantially less memory.
inline constexpr std::uint32_t maximum_warm_edit_preview_edge = 4'096;
// Full-detail sessions retain the provider's complete 16-bit reference RGB image, but never
// more than 512 MiB. The metadata preflight assumes worst-case RGB even when a provider may
// ultimately return one-channel grayscale data.
inline constexpr std::uint64_t maximum_full_edit_detail_retained_bytes =
    512ULL * 1'024ULL * 1'024ULL;
// Owned RawFrame development keeps scene-linear fp32 values so it can retain highlight headroom.
// A full-detail session therefore receives a larger, separate cap than packed RGB sources.
inline constexpr std::uint64_t maximum_full_edit_scene_linear_retained_bytes =
    1ULL * 1'024ULL * 1'024ULL * 1'024ULL;
// Detail work stays tile-local so one request cannot accidentally materialize another full-size
// float image while the immutable 16-bit source is resident.
inline constexpr std::uint32_t maximum_edit_detail_tile_side = 1'024;
inline constexpr std::uint32_t maximum_edit_detail_total_apron = 512;
inline constexpr std::uint32_t maximum_edit_detail_working_side = 2'048;

// An immutable, reusable scene-linear working proxy for interactive editing. Preparation is
// the only operation that asks DecodeSession to render the RAW. render_jpeg() owns all of its
// temporary edit/JPEG state, so concurrent const calls are safe after construction.
//
// The version-1 operations are pixel-local transforms in scene-linear RGB. Linear/affine nodes
// commute with the bilinear downsampling used to prepare this proxy. ToneCurve is nonlinear, so
// applying it here is an interactive proxy approximation rather than a bit-equivalent substitute
// for applying it before full-resolution downsampling. Masked or neighborhood operations must
// still declare an appropriate preview strategy rather than being silently routed through here.
inline constexpr std::size_t edit_preview_histogram_bin_count = 256U;
// Linear headroom is grouped in one-stop intervals above the SDR display-white reference:
// bin 0 is [1, 2), bin 1 is [2, 4), and the final bin is open-ended.  It is a compact
// scene/display-linear diagnostic for HDR readiness, not a sensor dynamic-range measurement.
inline constexpr std::size_t edit_preview_hdr_headroom_bin_count = 16U;
inline constexpr std::string_view edit_preview_analysis_version =
    "shadow.edit-preview-analysis.v2:rgb8-before-jpeg:rec709-encoded-q16:"
    "pre-clamp-linear-strict-lt-gt-any-channel:linear-headroom-log2-v1";
// Cache provenance for one completed warm-preview render. Adjustment and same-size display are
// independent provenance-bearing CPU/Metal stages. This belongs to the render result rather than
// the immutable session or generic EncodedProxy payload.
inline constexpr std::uint32_t edit_preview_execution_receipt_schema_version = 1U;
inline constexpr std::uint32_t edit_preview_cpu_adjustment_backend_version = 1U;
inline constexpr std::uint32_t edit_preview_metal_adjustment_backend_version = 1U;
inline constexpr std::uint32_t edit_preview_cpu_display_backend_version = 1U;
inline constexpr std::uint32_t edit_preview_metal_display_backend_version = 1U;
inline constexpr std::uint32_t edit_preview_jpeg_444_contract_version = 1U;

enum class EditPreviewBackend : std::uint8_t {
    cpu,
    metal,
};

struct EditPreviewExecutionReceipt final {
    std::uint32_t schema_version = edit_preview_execution_receipt_schema_version;
    EditPreviewBackend adjustment_backend = EditPreviewBackend::cpu;
    std::uint32_t adjustment_backend_version =
        edit_preview_cpu_adjustment_backend_version;
    std::uint32_t adjustment_execution_contract_version =
        edit_execution_plan_identity_version;
    EditPreviewBackend display_backend = EditPreviewBackend::cpu;
    std::uint32_t display_backend_version = edit_preview_cpu_display_backend_version;
    std::uint32_t display_output_contract_version =
        display_srgb8_output_transform_version;
    // The session-resident Metal route is structurally distinct from the
    // staged adjustment/display route. Keep that fact explicit instead of
    // encoding a route choice by inflating a backend version number.
    bool fused_pipeline = false;
    // These fields are diagnostic only. If automatic acceleration falls back, the complete
    // affected stage must restart from its immutable input; the effective CPU/Metal route above
    // then completely identifies the output math.
    bool adjustment_fell_back = false;
    bool display_fell_back = false;
    std::string diagnostic;

    [[nodiscard]] bool valid() const noexcept;
};

// Canonical cache-safe identity. It contains only fixed backend/contract identifiers; fallback
// diagnostics, local device information and user-local paths are deliberately excluded.
[[nodiscard]] std::string edit_preview_execution_receipt_identity(
    const EditPreviewExecutionReceipt& receipt
);

// Build/runtime-independent implementation contract known before a source is decoded. The
// desktop combines this with its bounded source-environment identity to reject stale gallery
// previews, while the per-render receipt above distinguishes the effective CPU/Metal route.
[[nodiscard]] std::string edit_preview_generator_implementation_identity();

// Transient analysis of one complete warm-proxy render. Histogram bins describe the uncompressed
// display-sRGB RGB8 pixels immediately before JPEG encoding. Clipping counts inspect the edited
// scene-linear values immediately before output clamping: exact 0 and 1 are legal, while a pixel
// is counted when any channel is below 0 or above 1. This is not sensor-domain exposure analysis.
struct EditPreviewAnalysis final {
    Dimensions sample_dimensions;
    std::array<std::uint64_t, edit_preview_histogram_bin_count> red{};
    std::array<std::uint64_t, edit_preview_histogram_bin_count> green{};
    std::array<std::uint64_t, edit_preview_histogram_bin_count> blue{};
    std::array<std::uint64_t, edit_preview_histogram_bin_count> luma{};
    std::array<std::uint64_t, 3> below_zero_samples{};
    std::array<std::uint64_t, 3> above_one_samples{};
    // Values are sampled after the edit graph and before the SDR display transform.  `1.0`
    // means display white, so positive EV values represent recoverable linear headroom.  The
    // peak is deliberately reported alongside the bins; it is a peak, not a percentile.
    std::array<std::uint64_t, edit_preview_hdr_headroom_bin_count> hdr_headroom_bins{};
    std::uint64_t hdr_headroom_pixels = 0;
    double hdr_peak_headroom_ev = 0.0;
    std::uint64_t pixel_count = 0;
    std::uint64_t shadow_clipped_pixels = 0;
    std::uint64_t highlight_clipped_pixels = 0;

    auto operator<=>(const EditPreviewAnalysis&) const = default;
};

struct AnalyzedEditPreview final {
    EncodedProxy proxy;
    EditPreviewAnalysis analysis;
    EditPreviewExecutionReceipt execution;
};

struct WarmEditPreviewGpuStats final {
    bool resident = false;
    std::uint64_t source_upload_count = 0U;
    std::uint64_t gpu_buffer_allocation_count = 0U;
    std::uint64_t render_count = 0U;
    std::uint64_t completed_render_count = 0U;
    std::uint64_t peak_concurrent_renders = 0U;
    std::uint64_t curve_resource_upload_count = 0U;
    std::uint64_t lut_resource_upload_count = 0U;
    std::uint64_t perceptual_mixer_resource_upload_count = 0U;
    std::uint64_t perceptual_range_resource_upload_count = 0U;
    std::uint64_t selective_color_resource_upload_count = 0U;
    std::uint64_t resource_cache_hit_count = 0U;
    std::uint64_t resident_bytes = 0U;

    auto operator<=>(const WarmEditPreviewGpuStats&) const = default;
};

template <typename T>
struct CancellableEditPreviewResult final {
    std::optional<T> completed;

    [[nodiscard]] bool cancelled() const noexcept {
        return !completed.has_value();
    }
};

namespace detail {
class WarmEditGpuSession;
}

class WarmEditPreviewSession final {
public:
    WarmEditPreviewSession(const WarmEditPreviewSession&) = delete;
    WarmEditPreviewSession& operator=(const WarmEditPreviewSession&) = delete;
    WarmEditPreviewSession(WarmEditPreviewSession&&) noexcept = default;
    WarmEditPreviewSession& operator=(WarmEditPreviewSession&&) noexcept = default;
    ~WarmEditPreviewSession() = default;

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint32_t max_edge() const noexcept;
    // Provenance of the provider render retained by this preview. It remains separate from the
    // editable recipe and from the later optical-correction receipt.
    [[nodiscard]] const RawDevelopmentReceipt& raw_development_receipt() const noexcept;
    [[nodiscard]] const RawPipelineReceipt& raw_pipeline_receipt() const noexcept;
    [[nodiscard]] const OpticsProfileReceipt& optics_receipt() const noexcept;
    // Optional source-domain clip classification retained from the same RawFrame development.
    // It never asks a provider to decode the source again merely to drive an optional zebra.
    [[nodiscard]] const std::optional<SensorClippingMask>& sensor_clipping_mask() const noexcept;
    // Runtime-only observability for tests and future diagnostics. These counters never enter
    // Recipe, catalog, or cache identities.
    [[nodiscard]] WarmEditPreviewGpuStats gpu_stats() const noexcept;
    [[nodiscard]] EncodedProxy render_jpeg(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] EncodedProxy render_jpeg_layers(
        std::span<const AdjustmentLayer> layers,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] AnalyzedEditPreview render_jpeg_with_analysis(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] AnalyzedEditPreview render_jpeg_with_analysis_layers(
        std::span<const AdjustmentLayer> layers,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<EncodedProxy> render_jpeg_cancellable(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<AnalyzedEditPreview>
    render_jpeg_with_analysis_cancellable(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {}
    ) const;

private:
    WarmEditPreviewSession(
        FloatRgbImage working_proxy,
        std::uint32_t max_edge,
        RawDevelopmentReceipt raw_development_receipt,
        RawPipelineReceipt raw_pipeline_receipt,
        OpticsProfileReceipt optics_receipt,
        std::optional<SensorClippingMask> sensor_clipping_mask
    );

    FloatRgbImage working_proxy_;
    std::uint32_t max_edge_ = 0;
    RawDevelopmentReceipt raw_development_receipt_;
    RawPipelineReceipt raw_pipeline_receipt_;
    OpticsProfileReceipt optics_receipt_;
    std::optional<SensorClippingMask> sensor_clipping_mask_;
    std::shared_ptr<detail::WarmEditGpuSession> warm_gpu_session_;
    std::string warm_gpu_diagnostic_;

    friend WarmEditPreviewSession prepare_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge,
        const OpticsProvider* optics_provider,
        const OpticsSettings& optics_settings
    );
    friend WarmEditPreviewSession prepare_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge,
        const RawDevelopmentPlan& raw_development_plan,
        const OpticsProvider* optics_provider,
        const OpticsSettings& optics_settings
    );
};

struct DetailTileRect final {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    auto operator<=>(const DetailTileRect&) const = default;
};

// Packed, display-referred sRGB bytes for one exact full-resolution rectangle. Rows carry no
// padding and no compression is applied, avoiding independently encoded JPEG block or chroma
// boundaries between neighboring tiles.
struct RenderedDetailTile final {
    DetailTileRect rect;
    Dimensions full_dimensions;
    std::uint32_t row_stride_bytes = 0;
    std::vector<std::uint8_t> bytes;
};

// An immutable complete linear source for 1:1 detail requests. Raster/provider-compatibility
// sources retain packed u16 RGB; Shadow's owned RawFrame route retains scene-linear fp32 so
// highlight headroom survives until the requested tile reaches the edit graph.
class FullEditDetailSession final {
public:
    FullEditDetailSession(const FullEditDetailSession&) = delete;
    FullEditDetailSession& operator=(const FullEditDetailSession&) = delete;
    FullEditDetailSession(FullEditDetailSession&&) noexcept = default;
    FullEditDetailSession& operator=(FullEditDetailSession&&) noexcept = default;
    ~FullEditDetailSession() = default;

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] const RawDevelopmentReceipt& raw_development_receipt() const noexcept;
    [[nodiscard]] const RawPipelineReceipt& raw_pipeline_receipt() const noexcept;
    [[nodiscard]] const OpticsProfileReceipt& optics_receipt() const noexcept;
    [[nodiscard]] RenderedDetailTile render_rgb8(
        std::span<const AdjustmentNode> nodes,
        DetailTileRect rect,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] RenderedDetailTile render_rgb8_layers(
        std::span<const AdjustmentLayer> layers,
        DetailTileRect rect,
        const PhotoGeometry& geometry = {}
    ) const;

private:
    FullEditDetailSession(
        DevelopedSourcePixels reference_source,
        std::uint64_t retained_bytes,
        RawDevelopmentReceipt raw_development_receipt,
        RawPipelineReceipt raw_pipeline_receipt,
        OpticsProfileReceipt optics_receipt,
        SourceRenderingReceipt source_rendering
    );

    DevelopedSourcePixels reference_source_;
    std::uint64_t retained_bytes_ = 0;
    // Kept separately from the post-optics raster: an independently implemented OpticsProvider
    // is allowed to allocate a new PixelBuffer and must not be able to erase decoder provenance.
    RawDevelopmentReceipt raw_development_receipt_;
    RawPipelineReceipt raw_pipeline_receipt_;
    OpticsProfileReceipt optics_receipt_;
    // Source rendering is independent from the editable Recipe. Retain its compact receipt so
    // full-resolution tiles apply the exact same standard/profile exposure as the warm proxy.
    SourceRenderingReceipt source_rendering_;

    friend FullEditDetailSession prepare_full_edit_detail(
        const DecodeSession& session,
        const OpticsProvider* optics_provider,
        const OpticsSettings& optics_settings
    );
    friend FullEditDetailSession prepare_full_edit_detail(
        const DecodeSession& session,
        const RawDevelopmentPlan& raw_development_plan,
        const OpticsProvider* optics_provider,
        const OpticsSettings& optics_settings
    );
};

// Decodes processed linear-light sRGB-primary u16 once and stores only a max-edge-bounded linear
// float proxy. Normalization precedes bilinear downsampling; no transfer is decoded and the
// full-size float image is never materialized.
[[nodiscard]] WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    std::uint32_t max_edge = 2'048,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// Same bounded preview contract, but with an explicit source-development request. The plan is
// not a user-visible adjustment node: it controls how a RAW provider produces the immutable
// source raster before the common RGB edit graph. JPEG/HEIF providers intentionally keep their
// legacy path because their pixels have already been developed.
[[nodiscard]] WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// Checks the provider metadata against the worst-case RGB u16 retention bound before asking it
// to render pixels, then independently checks the actual retained vector allocation.
[[nodiscard]] FullEditDetailSession prepare_full_edit_detail(
    const DecodeSession& session,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// Requests the immutable source at full-resolution detail or export intent. A RAW provider may
// negotiate quality or policy details, but it must retain the requested/effective plan pair in
// the returned RawDevelopmentReceipt. Preview intent is rejected so a low-cost warm raster can
// never accidentally populate a 1:1 or export cache entry.
[[nodiscard]] FullEditDetailSession prepare_full_edit_detail(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

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
[[nodiscard]] EncodedProxy render_reference_proxy_jpeg(
    const DecodeSession& session,
    ProxyRequest request,
    const RawDevelopmentPlan& raw_development_plan
);

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
