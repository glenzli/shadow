#pragma once

#include "rust/cxx.h"

namespace shadow::bridge {
class DecodeHandle;
class EditPreviewHandle;
class FullEditDetailHandle;
}

#include "shadow-bridge/src/lib.rs.h"

#include <shadow/image/decoder.hpp>
#include <shadow/image/display_luma.hpp>
#include <shadow/image/edit.hpp>

#include <memory>

namespace shadow::bridge {

class DecodeHandle final {
public:
    DecodeHandle(
        std::unique_ptr<image::DecoderProvider> provider,
        std::unique_ptr<image::DecodeSession> session,
        std::shared_ptr<const image::OpticsProvider> optics_provider
    );
    ~DecodeHandle();

    DecodeHandle(const DecodeHandle&) = delete;
    DecodeHandle& operator=(const DecodeHandle&) = delete;

    [[nodiscard]] FfiProviderSnapshot provider() const;
    [[nodiscard]] FfiMetadataSnapshot metadata() const;
    [[nodiscard]] FfiCapabilitySnapshot capabilities() const;
    [[nodiscard]] FfiRawDevelopmentCapabilities raw_development_capabilities() const;
    [[nodiscard]] FfiRawDevelopmentPlanNegotiation negotiate_raw_development_plan(
        const FfiRawDevelopmentPlan& plan
    ) const;
    // The last prepared RAW source render. It is explicitly empty until a render-backed edit
    // session is prepared, rather than causing a hidden second RAW decode merely for metadata.
    [[nodiscard]] FfiRawDevelopmentReceipt raw_development_receipt() const;
    // Host-side route provenance is kept separate from provider-side RAW development. This
    // identifies whether the prepared source used Shadow's RawFrame developer, a decoded raster,
    // or an explicit provider-processed compatibility path.
    [[nodiscard]] FfiRawPipelineReceipt raw_pipeline_receipt() const;
    [[nodiscard]] rust::Vec<FfiPreviewSnapshot> previews() const;
    [[nodiscard]] FfiPreviewPayload decode_best_preview();
    void configure_optics(const FfiOpticsSettings& settings);
    [[nodiscard]] FfiEncodedProxy render_reference_proxy(
        std::uint32_t max_edge,
        std::uint8_t jpeg_quality
    ) const;
    [[nodiscard]] FfiEncodedProxy render_adjustment_plan(
        const FfiAdjustmentRenderRequest& request
    ) const;
    // A compact, source-invariant RAW diagnostic. It is intentionally produced from the
    // provider-owned RawFrame before the immutable RGB preview is retained, so slider renders
    // never need to unpack the sensor again.
    [[nodiscard]] FfiSensorClippingMask sensor_clipping_mask(
        std::uint32_t target_width,
        std::uint32_t target_height
    ) const;
    [[nodiscard]] std::unique_ptr<EditPreviewHandle> prepare_edit_preview(
        std::uint32_t max_edge
    ) const;
    [[nodiscard]] std::unique_ptr<EditPreviewHandle>
    prepare_edit_preview_with_raw_development_plan(
        std::uint32_t max_edge,
        const FfiRawDevelopmentPlan& plan
    ) const;
    [[nodiscard]] std::unique_ptr<FullEditDetailHandle> prepare_edit_detail() const;
    [[nodiscard]] std::unique_ptr<FullEditDetailHandle>
    prepare_edit_detail_with_raw_development_plan(
        const FfiRawDevelopmentPlan& plan
    ) const;

private:
    std::unique_ptr<image::DecoderProvider> provider_;
    std::unique_ptr<image::DecodeSession> session_;
    std::shared_ptr<const image::OpticsProvider> optics_provider_;
    image::OpticsSettings optics_settings_;
    mutable image::RawDevelopmentReceipt raw_development_receipt_;
    mutable image::RawPipelineReceipt raw_pipeline_receipt_{.schema_version = 0U};
};

// Unlike DecodeHandle, this handle no longer owns or references a decoder. Its working proxy
// is immutable after preparation and render_adjustment_plan() uses only call-local state, so const
// calls may safely run concurrently on different worker threads.
class EditPreviewHandle final {
public:
    explicit EditPreviewHandle(image::WarmEditPreviewSession session);
    ~EditPreviewHandle();

    EditPreviewHandle(const EditPreviewHandle&) = delete;
    EditPreviewHandle& operator=(const EditPreviewHandle&) = delete;

    [[nodiscard]] FfiDimensions dimensions() const noexcept;
    [[nodiscard]] std::uint32_t max_edge() const noexcept;
    [[nodiscard]] FfiOpticsReceipt optics_receipt() const;
    [[nodiscard]] FfiRawDevelopmentReceipt raw_development_receipt() const;
    [[nodiscard]] FfiRawPipelineReceipt raw_pipeline_receipt() const;
    [[nodiscard]] FfiEncodedProxy render_adjustment_plan(
        const FfiAdjustmentRenderRequest& request
    ) const;
    [[nodiscard]] FfiAnalyzedEditPreview render_adjustment_plan_with_analysis(
        const FfiAdjustmentRenderRequest& request
    ) const;

private:
    image::WarmEditPreviewSession session_;
};

// The complete retained source is immutable and contains no decoder. Every tile render owns its
// float working buffer and packed RGB8 result, so const calls may safely run concurrently.
class FullEditDetailHandle final {
public:
    explicit FullEditDetailHandle(image::FullEditDetailSession session);
    ~FullEditDetailHandle();

    FullEditDetailHandle(const FullEditDetailHandle&) = delete;
    FullEditDetailHandle& operator=(const FullEditDetailHandle&) = delete;

    [[nodiscard]] FfiDimensions dimensions() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] FfiOpticsReceipt optics_receipt() const;
    [[nodiscard]] FfiRawDevelopmentReceipt raw_development_receipt() const;
    [[nodiscard]] FfiRawPipelineReceipt raw_pipeline_receipt() const;
    [[nodiscard]] FfiRenderedDetailTile render_adjustment_plan_tile(
        const FfiAdjustmentDetailTileRequest& request
    ) const;

private:
    image::FullEditDetailSession session_;
};

[[nodiscard]] std::unique_ptr<DecodeHandle> open_libraw_utf8(rust::Str path);
[[nodiscard]] std::unique_ptr<DecodeHandle> open_photo_utf8(rust::Str path);
[[nodiscard]] rust::Vec<FfiOpticsProfileCandidate> query_libraw_optics_profiles_utf8(
    rust::Str path
);
[[nodiscard]] rust::Vec<FfiOpticsProfileCandidate> query_photo_optics_profiles_utf8(
    rust::Str path
);
// Enumerates RAW optical profiles from metadata already persisted by the Catalog. This path is
// deliberately independent from pixel decode: an unsupported RAW compression may still expose
// complete camera/lens EXIF through an earlier metadata inspection or a private provider.
[[nodiscard]] rust::Vec<FfiOpticsProfileCandidate> query_optics_profiles_for_metadata(
    const FfiMetadataSnapshot& metadata
);
[[nodiscard]] rust::String libraw_provider_version();
[[nodiscard]] rust::String photo_provider_version();
[[nodiscard]] rust::Vec<rust::String> photo_supported_raster_extensions();
[[nodiscard]] rust::String raw_development_plan_identity(const FfiRawDevelopmentPlan& plan);
[[nodiscard]] FfiEncodedProxy render_photo_reference_proxy(
    rust::Str path,
    std::uint32_t max_edge,
    std::uint8_t jpeg_quality
);
[[nodiscard]] FfiDisplayLuma decode_jpeg_display_luma(
    rust::Slice<const std::uint8_t> encoded,
    std::uint32_t max_edge
);

} // namespace shadow::bridge
