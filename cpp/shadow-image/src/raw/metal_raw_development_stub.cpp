#include "metal_raw_development.hpp"

namespace shadow::image::detail {

struct MetalRawPreviewResidentOutput::Impl final {};

MetalRawPreviewResidentOutput::MetalRawPreviewResidentOutput(
    std::unique_ptr<Impl> implementation
) noexcept :
    implementation_(std::move(implementation)) {}

MetalRawPreviewResidentOutput::MetalRawPreviewResidentOutput(
    MetalRawPreviewResidentOutput&&
) noexcept = default;

MetalRawPreviewResidentOutput& MetalRawPreviewResidentOutput::operator=(
    MetalRawPreviewResidentOutput&&
) noexcept = default;

MetalRawPreviewResidentOutput::~MetalRawPreviewResidentOutput() = default;

Dimensions MetalRawPreviewResidentOutput::dimensions() const noexcept {
    return {};
}

std::size_t MetalRawPreviewResidentOutput::row_stride_bytes() const noexcept {
    return 0U;
}

std::uint64_t MetalRawPreviewResidentOutput::output_bytes() const noexcept {
    return 0U;
}

std::uint64_t MetalRawPreviewResidentOutput::external_resident_bytes() const noexcept {
    return 0U;
}

std::uint64_t MetalRawPreviewResidentOutput::resident_allowance_bytes() const noexcept {
    return 0U;
}

void* MetalRawPreviewResidentOutput::native_device_handle() const noexcept {
    return nullptr;
}

void* MetalRawPreviewResidentOutput::native_queue_handle() const noexcept {
    return nullptr;
}

void* MetalRawPreviewResidentOutput::native_buffer_handle() const noexcept {
    return nullptr;
}

struct MetalRawPreviewRebindingSource::Impl final {};

MetalRawPreviewRebindingSource::MetalRawPreviewRebindingSource(
    std::unique_ptr<Impl> implementation
) noexcept :
    implementation_(std::move(implementation)) {}

MetalRawPreviewRebindingSource::MetalRawPreviewRebindingSource(
    MetalRawPreviewRebindingSource&&
) noexcept = default;

MetalRawPreviewRebindingSource& MetalRawPreviewRebindingSource::operator=(
    MetalRawPreviewRebindingSource&&
) noexcept = default;

MetalRawPreviewRebindingSource::~MetalRawPreviewRebindingSource() = default;

std::optional<MetalRawPreviewRebindingSource> MetalRawPreviewRebindingSource::try_prepare(
    const RawFrame&,
    std::string& diagnostic
) {
    diagnostic = "Metal RAW preview rebinding is not compiled for this platform";
    return std::nullopt;
}

MetalRawDevelopmentAttempt MetalRawPreviewRebindingSource::develop(
    const RawFrame&,
    const RawFrameLinearTransform&,
    const std::optional<std::uint32_t>,
    const RawHighlightRecoveryIntent,
    const RawDevelopmentQuality,
    const MetalRawDevelopmentContinuations
) const {
    return MetalRawDevelopmentAttempt{
        .development = std::nullopt,
        .sensor_clipping_mask = std::nullopt,
        .raw_denoise_applied = false,
        .dcp_applied = false,
        .diagnostic = "Metal RAW preview rebinding is not compiled for this platform",
    };
}

MetalRawPreviewResidentDevelopmentAttempt MetalRawPreviewRebindingSource::develop_resident(
    const RawFrame&,
    const RawFrameLinearTransform&,
    const std::optional<std::uint32_t>,
    const RawHighlightRecoveryIntent,
    const RawDevelopmentQuality,
    const MetalRawDevelopmentContinuations
) const {
    return MetalRawPreviewResidentDevelopmentAttempt{
        .output = std::nullopt,
        .demosaic_receipt = {},
        .dcp_applied = false,
        .diagnostic = "Metal RAW preview rebinding is not compiled for this platform",
    };
}

bool metal_raw_development_available() noexcept {
    return false;
}

const std::string& metal_raw_development_diagnostic() noexcept {
    static const std::string diagnostic = "Metal RAW development is not compiled for this platform";
    return diagnostic;
}

bool metal_raw_denoise_available() noexcept {
    return false;
}

bool metal_dcp_color_development_available() noexcept {
    return false;
}

MetalRawDenoiseAttempt
try_denoise_bayer_raw_frame_metal(RawFrame&, const RawBayerDenoiseMode, const double) {
    return MetalRawDenoiseAttempt{
        .applied = false,
        .diagnostic = "Metal RAW denoise is not compiled for this platform",
    };
}

MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_f32_metal(
    const RawFrame&,
    const RawFrameLinearTransform&,
    const std::optional<std::uint32_t>,
    const RawHighlightRecoveryIntent,
    const RawDevelopmentQuality,
    const MetalRawDevelopmentContinuations
) {
    return MetalRawDevelopmentAttempt{
        .development = std::nullopt,
        .sensor_clipping_mask = std::nullopt,
        .raw_denoise_applied = false,
        .dcp_applied = false,
        .diagnostic = "Metal RAW development is not compiled for this platform",
    };
}

MetalDcpColorDevelopmentAttempt
try_apply_dcp_color_rendering_stages_metal(SceneLinearRgbFrame&, const DcpColorTransform&) {
    return MetalDcpColorDevelopmentAttempt{
        .applied = false,
        .diagnostic = "Metal DCP color development is not compiled for this platform",
    };
}

} // namespace shadow::image::detail
