#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/decoder_types.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/edit_preview_frame.hpp>
#include <shadow/image/optics.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_development_receipt.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/raw_white_balance.hpp>
#include <shadow/image/reference_pixels.hpp>
#include <shadow/image/sensor_clipping.hpp>
#include <shadow/image/working_rgb.hpp>

#include <array>
#include <compare>
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

class DecodeSession;
struct PhotoLiquify;

// A square proxy at this limit occupies at most 192 MiB as interleaved RGB float32.
// Typical 3:2 photos at the UI's 1600/2048 edge use substantially less memory.
inline constexpr std::uint32_t maximum_warm_edit_preview_edge = 4'096;

// An immutable, reusable scene-linear working proxy for interactive editing. Preparation is
// the only operation that asks DecodeSession to render the RAW. Each render owns all temporary
// edit/output state, so concurrent const calls are safe after construction. Interactive callers
// use the transient RGB8 route; settled analysis and durable proxy callers use JPEG.
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
    std::uint32_t adjustment_backend_version = edit_preview_cpu_adjustment_backend_version;
    std::uint32_t adjustment_execution_contract_version = edit_execution_plan_identity_version;
    EditPreviewBackend display_backend = EditPreviewBackend::cpu;
    std::uint32_t display_backend_version = edit_preview_cpu_display_backend_version;
    std::uint32_t display_output_contract_version = display_srgb8_output_transform_version;
    // The session-resident Metal route is structurally distinct from the
    // staged adjustment/display route. Keep that fact explicit instead of
    // encoding a route choice by inflating a backend version number.
    bool fused_pipeline = false;
    // These fields are diagnostic only. If automatic acceleration falls back, the complete
    // affected stage must restart from its immutable input; the effective CPU/Metal route above
    // then completely identifies the output math.
    bool adjustment_fell_back = false;
    bool display_fell_back = false;
    bool presentation_fell_back = false;
    std::string diagnostic;

    [[nodiscard]] bool valid() const noexcept;
};

// Canonical cache-safe identity. It contains only fixed backend/contract identifiers; fallback
// diagnostics, local device information and user-local paths are deliberately excluded.
[[nodiscard]] std::string
edit_preview_execution_receipt_identity(const EditPreviewExecutionReceipt& receipt);

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

struct EditPreviewRgb8WithMaskCoverage final {
    EncodedProxy preview;
    std::optional<EditPreviewMaskCoverage> mask_coverage;
};

struct AnalyzedEditPreviewWithMaskCoverage final {
    AnalyzedEditPreview preview;
    std::optional<EditPreviewMaskCoverage> mask_coverage;
};

struct WarmEditPreviewGpuStats final {
    bool resident = false;
    std::uint64_t source_upload_count = 0U;
    std::uint64_t gpu_buffer_allocation_count = 0U;
    std::uint64_t render_count = 0U;
    std::uint64_t completed_render_count = 0U;
    std::uint64_t peak_concurrent_renders = 0U;
    // Presentation counters describe only explicit native-surface requests. A completed request
    // either publishes one independently owned texture or records a named host-RGB fallback.
    std::uint64_t presentation_surface_request_count = 0U;
    std::uint64_t presentation_surface_publish_count = 0U;
    std::uint64_t presentation_surface_fallback_count = 0U;
    std::uint64_t curve_resource_upload_count = 0U;
    std::uint64_t lut_resource_upload_count = 0U;
    std::uint64_t perceptual_mixer_resource_upload_count = 0U;
    std::uint64_t perceptual_range_resource_upload_count = 0U;
    std::uint64_t selective_color_resource_upload_count = 0U;
    std::uint64_t brush_index_resource_upload_count = 0U;
    std::uint64_t retouch_geometry_resource_upload_count = 0U;
    std::uint64_t resource_cache_hit_count = 0U;
    std::uint64_t resident_bytes = 0U;

    auto operator<=>(const WarmEditPreviewGpuStats&) const = default;
};

template <typename T> struct CancellableEditPreviewResult final {
    std::optional<T> completed;

    [[nodiscard]] bool cancelled() const noexcept {
        return !completed.has_value();
    }
};

namespace detail {
class WarmEditGpuSession;
}
namespace raw_pipeline_detail {
class RawPreviewRebindingSource;
struct RawPreviewRebindingTelemetry;
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
    [[nodiscard]] const std::optional<HighlightChromaRiskMap>& highlight_chroma_risk_map()
        const noexcept;
    // Runtime-only observability for tests and future diagnostics. These counters never enter
    // Recipe, catalog, or cache identities.
    [[nodiscard]] WarmEditPreviewGpuStats gpu_stats() const noexcept;
    // RAW-only fast path. A rebound session shares the immutable decoded/denoised camera-space
    // foundation but owns a fresh scene-linear proxy, DCP receipt and GPU edit session. Raster
    // sessions and legacy borrowed-optics preparations deliberately report false.
    [[nodiscard]] bool supports_raw_development_rebinding() const noexcept;
    // A sensor-domain RAW neutral picker is intentionally narrower than
    // rebinding: it requires an ordinary retained CFA frame, not an RGB
    // compatibility or AI-foundation representation.
    [[nodiscard]] bool supports_raw_white_balance_picker() const noexcept;
    [[nodiscard]] std::optional<RawWhiteBalancePresentation>
    pick_raw_white_balance(double normalized_x, double normalized_y) const noexcept;
    [[nodiscard]] std::optional<RawWhiteBalancePresentation>
    auto_raw_white_balance() const noexcept;
    // Source-stage observability stays attached to the retained camera basis,
    // so a new immutable session can report cumulative rebind execution
    // without polluting Recipe/cache provenance.
    [[nodiscard]] raw_pipeline_detail::RawPreviewRebindingTelemetry
    raw_rebinding_telemetry() const noexcept;
    [[nodiscard]] WarmEditPreviewSession
    rebind_raw_development_plan(const RawDevelopmentPlan& raw_development_plan) const;
    // AI-only bounded-source fast path. The new session shares the retained original/AI
    // camera-space basis and performs no source decode, artifact read, or model execution.
    [[nodiscard]] bool supports_raw_foundation_amount_rebinding() const noexcept;
    [[nodiscard]] WarmEditPreviewSession rebind_raw_foundation_amount(
        const RawDevelopmentPlan& raw_development_plan,
        std::uint8_t amount_percent
    ) const;
    // Interactive presentation path. The returned Bitmap payload is tightly
    // packed display-sRGB RGB8 (`width * 3` bytes per row) and deliberately
    // skips JPEG encoding. It remains transient and is never a durable cache
    // artifact.
    [[nodiscard]] EncodedProxy render_rgb8(
        std::span<const AdjustmentNode> nodes,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] EncodedProxy render_rgb8_layers(
        std::span<const AdjustmentLayer> layers,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] EncodedProxy render_jpeg(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] EncodedProxy render_jpeg_layers(
        std::span<const AdjustmentLayer> layers,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] AnalyzedEditPreview render_jpeg_with_analysis(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] AnalyzedEditPreview render_jpeg_with_analysis_layers(
        std::span<const AdjustmentLayer> layers,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<EncodedProxy> render_jpeg_cancellable(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<EncodedProxy> render_rgb8_cancellable(
        std::span<const AdjustmentNode> nodes,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    // Opaque-owner interactive route. On compatible Metal systems the returned frame owns a
    // native presentation surface and has no materialized host RGB8 bytes. CPU and named
    // presentation fallbacks retain the exact packed RGB8 contract.
    [[nodiscard]] CancellableEditPreviewResult<InteractiveEditPreviewFrame>
    render_interactive_frame_cancellable(
        std::span<const AdjustmentNode> nodes,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<EncodedProxy> render_rgb8_layers_cancellable(
        std::span<const AdjustmentLayer> layers,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<EditPreviewRgb8WithMaskCoverage>
    render_rgb8_layers_with_mask_coverage_cancellable(
        std::span<const AdjustmentLayer> layers,
        std::optional<std::uint32_t> target_layer_index,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<InteractiveEditPreviewFrame>
    render_interactive_frame_layers_with_mask_coverage_cancellable(
        std::span<const AdjustmentLayer> layers,
        std::optional<std::uint32_t> target_layer_index,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<AnalyzedEditPreview>
    render_jpeg_with_analysis_cancellable(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<AnalyzedEditPreviewWithMaskCoverage>
    render_jpeg_with_analysis_layers_and_mask_coverage_cancellable(
        std::span<const AdjustmentLayer> layers,
        std::optional<std::uint32_t> target_layer_index,
        std::uint8_t jpeg_quality,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {},
        const PhotoLiquify* liquify = nullptr
    ) const;

  private:
    WarmEditPreviewSession(
        FloatRgbImage working_proxy,
        std::uint32_t max_edge,
        RawDevelopmentReceipt raw_development_receipt,
        RawPipelineReceipt raw_pipeline_receipt,
        OpticsProfileReceipt optics_receipt,
        std::optional<SensorClippingMask> sensor_clipping_mask,
        std::optional<HighlightChromaRiskMap> highlight_chroma_risk_map,
        std::shared_ptr<const raw_pipeline_detail::RawPreviewRebindingSource> raw_rebinding_source =
            nullptr,
        std::shared_ptr<const OpticsProvider> retained_optics_provider = nullptr,
        OpticsSettings retained_optics_settings = default_optics_settings(),
        std::shared_ptr<detail::WarmEditGpuSession> adopted_warm_gpu_session = nullptr
    );

    FloatRgbImage working_proxy_;
    std::uint32_t max_edge_ = 0;
    RawDevelopmentReceipt raw_development_receipt_;
    RawPipelineReceipt raw_pipeline_receipt_;
    OpticsProfileReceipt optics_receipt_;
    std::optional<SensorClippingMask> sensor_clipping_mask_;
    std::optional<HighlightChromaRiskMap> highlight_chroma_risk_map_;
    std::shared_ptr<detail::WarmEditGpuSession> warm_gpu_session_;
    std::string warm_gpu_diagnostic_;
    std::shared_ptr<const raw_pipeline_detail::RawPreviewRebindingSource> raw_rebinding_source_;
    std::shared_ptr<const OpticsProvider> retained_optics_provider_;
    OpticsSettings retained_optics_settings_;

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
    friend WarmEditPreviewSession prepare_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge,
        const RawDevelopmentPlan& raw_development_plan,
        const RawFoundationCameraRgbView& foundation,
        const OpticsProvider* optics_provider,
        const OpticsSettings& optics_settings
    );
    friend WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge,
        const RawDevelopmentPlan& raw_development_plan,
        std::shared_ptr<const OpticsProvider> optics_provider,
        const OpticsSettings& optics_settings
    );
    friend WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
        const DecodeSession& metadata_session,
        std::uint32_t max_edge,
        const RawDevelopmentPlan& raw_development_plan,
        RawFrame staged_frame,
        std::shared_ptr<const OpticsProvider> optics_provider,
        const OpticsSettings& optics_settings
    );
    friend WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
        const AssetMetadata& metadata,
        std::uint32_t max_edge,
        const RawDevelopmentPlan& raw_development_plan,
        RawFrame staged_frame,
        std::shared_ptr<const OpticsProvider> optics_provider,
        const OpticsSettings& optics_settings
    );
    friend WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge,
        const RawDevelopmentPlan& raw_development_plan,
        const RawFoundationCameraRgbView& foundation,
        RawFrame staged_frame,
        std::shared_ptr<const OpticsProvider> optics_provider,
        const OpticsSettings& optics_settings
    );
    friend WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge,
        const RawDevelopmentPlan& raw_development_plan,
        const RawFoundationCameraRgbView& foundation,
        std::shared_ptr<const OpticsProvider> optics_provider,
        const OpticsSettings& optics_settings
    );
};

// Bridge-facing ownership-preserving preparation. The ordinary pointer overloads remain useful
// to focused native callers, while desktop sessions use these entry points so a later RAW colour
// rebind can safely reapply the same immutable optics provider without borrowing DecodeHandle.
[[nodiscard]] WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const DecodeSession& session,
    std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// Uses one helper-staged provider-neutral sensor frame while the in-process
// session supplies metadata, camera profiles, and optics only. This is the
// ordinary (non-AI-foundation) isolated route used when authored RAW white
// balance cannot be reproduced by a provider-processed RGB fallback.
[[nodiscard]] WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const DecodeSession& metadata_session,
    std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    RawFrame staged_frame,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// A provider-neutral RawFrame and metadata snapshot emitted by the isolated
// helper form one source-admission unit. No source path or decoder session is
// required after that unit is established.
[[nodiscard]] WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const AssetMetadata& metadata,
    std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    RawFrame staged_frame,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings = default_optics_settings()
);

[[nodiscard]] WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const DecodeSession& metadata_session,
    std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    RawFrame staged_frame,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings = default_optics_settings()
);

[[nodiscard]] WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const DecodeSession& session,
    std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings = default_optics_settings()
);

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

// Prepares the same bounded edit surface from one verified AI RAW foundation. This overload is
// explicit so an enabled node cannot reuse the ordinary RawFrame source by accident. Foundation
// identity remains available through `raw_pipeline_receipt()` for the caller's session/cache key.
[[nodiscard]] WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

} // namespace shadow::image
