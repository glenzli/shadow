#include "full_edit_detail_metal_source.hpp"

namespace shadow::image::detail {

FullEditDetailMetalSourcePreparation prepare_full_edit_detail_metal_source(
    raw_pipeline_detail::ResidentRawSource& source,
    const SourceRenderingReceipt&,
    GeometryPixelRect,
    std::uint64_t
) {
    source.invalidate_device_path();
    return FullEditDetailMetalSourcePreparation{
        .session = nullptr,
        .telemetry = {},
        .diagnostic = "full-detail Metal RAW source rendering is unavailable on this platform",
    };
}

bool full_edit_detail_metal_source_available() noexcept {
    return false;
}

const std::string& full_edit_detail_metal_source_diagnostic() noexcept {
    static const std::string diagnostic =
        "full-detail Metal RAW source rendering is unavailable on this platform";
    return diagnostic;
}

} // namespace shadow::image::detail
