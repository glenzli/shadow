#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/decoder_types.hpp>
#include <shadow/image/optics.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_development_receipt.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/source_rendering.hpp>

#include <compare>
#include <cstdint>
#include <span>
#include <vector>

namespace shadow::image {

class DecodeSession;

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

} // namespace shadow::image
