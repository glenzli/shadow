#pragma once

#include <shadow/image/cxx_bridge.hpp>

namespace shadow::bridge::cxx_bridge_projection {

[[nodiscard]] FfiDimensions dimensions(image::Dimensions value) noexcept;
[[nodiscard]] FfiByteOrder byte_order(image::ByteOrder value) noexcept;
[[nodiscard]] FfiPreviewSnapshot preview_snapshot(
    const image::PreviewDescriptor& preview
);
[[nodiscard]] FfiEncodedProxy encoded_proxy(const image::EncodedProxy& proxy);
[[nodiscard]] FfiSensorClippingMask sensor_clipping_mask(
    const image::SensorClippingMask& mask
);
[[nodiscard]] FfiAnalyzedEditPreview analyzed_edit_preview(
    const image::AnalyzedEditPreview& preview
);
[[nodiscard]] FfiEditPreviewMaskCoverage edit_preview_mask_coverage(
    const std::optional<image::EditPreviewMaskCoverage>& coverage
);
[[nodiscard]] image::DetailTileRect detail_tile_rect(
    const FfiDetailTileRect& value
) noexcept;
[[nodiscard]] image::PhotoGeometry photo_geometry(
    const FfiPhotoGeometry& value
);
[[nodiscard]] FfiRenderedDetailTile rendered_detail_tile(
    const image::RenderedDetailTile& tile
);
[[nodiscard]] FfiOpticsReceipt optics_receipt(
    const image::OpticsProfileReceipt& receipt
);
[[nodiscard]] image::RawDevelopmentPlan raw_development_plan(
    FfiRawDevelopmentPlan const& plan
);
[[nodiscard]] FfiRawDevelopmentCapabilities raw_development_capabilities(
    const image::RawDevelopmentCapabilities& capabilities
) noexcept;
[[nodiscard]] FfiRawDevelopmentPlanNegotiation raw_development_plan_negotiation(
    const image::RawDevelopmentPlanNegotiation& negotiation
);
[[nodiscard]] FfiRawDevelopmentReceipt raw_development_receipt(
    const image::RawDevelopmentReceipt& receipt
);
[[nodiscard]] FfiRawPipelineReceipt raw_pipeline_receipt(
    const image::RawPipelineReceipt& receipt
);

} // namespace shadow::bridge::cxx_bridge_projection
