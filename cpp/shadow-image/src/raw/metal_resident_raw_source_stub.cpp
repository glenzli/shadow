#include "metal_resident_raw_source.hpp"

#include "resident_raw_source.hpp"

#include <shadow/image/decoder_error.hpp>

#include <utility>

namespace shadow::image::raw_pipeline_detail {

struct MetalResidentRawSource::Impl final {
    RawBayerDenoiseReceipt raw_denoise_receipt;
};

struct MetalResidentRawRegionLease::Impl final {};

MetalResidentRawRegionLease::MetalResidentRawRegionLease(
    std::unique_ptr<Impl> implementation
) noexcept : implementation_(std::move(implementation)) {}

MetalResidentRawRegionLease::MetalResidentRawRegionLease(MetalResidentRawRegionLease&&) noexcept =
    default;

MetalResidentRawRegionLease&
MetalResidentRawRegionLease::operator=(MetalResidentRawRegionLease&&) noexcept = default;

MetalResidentRawRegionLease::~MetalResidentRawRegionLease() = default;

Dimensions MetalResidentRawRegionLease::dimensions() const noexcept {
    return {};
}

std::uint64_t MetalResidentRawRegionLease::device_identity() const noexcept {
    return 0U;
}

std::uint64_t MetalResidentRawRegionLease::retained_bytes() const noexcept {
    return 0U;
}

std::uint64_t MetalResidentRawRegionLease::completion_fence_value() const noexcept {
    return 0U;
}

DevelopedMetalResidentRawRegion MetalResidentRawRegionLease::readback_compatibility() const {
    throw DecodeError(
        DecodeErrorCode::unsupported,
        0,
        "Metal resident RAW source is unavailable on this platform"
    );
}

void* MetalResidentRawRegionLease::native_buffer_handle() const noexcept {
    return nullptr;
}

void* MetalResidentRawRegionLease::native_device_handle() const noexcept {
    return nullptr;
}

GeometryPixelRect MetalResidentRawRegionLease::requested_core() const noexcept {
    return {};
}

Dimensions MetalResidentRawRegionLease::full_dimensions() const noexcept {
    return {};
}

void MetalResidentRawRegionLease::invalidate_source() const noexcept {}

MetalResidentRawSource::MetalResidentRawSource(std::shared_ptr<Impl> implementation) noexcept :
    implementation_(std::move(implementation)) {}

MetalResidentRawSource::MetalResidentRawSource(MetalResidentRawSource&&) noexcept = default;

MetalResidentRawSource&
MetalResidentRawSource::operator=(MetalResidentRawSource&&) noexcept = default;

MetalResidentRawSource::~MetalResidentRawSource() = default;

Dimensions MetalResidentRawSource::dimensions() const noexcept {
    return {};
}

std::uint64_t MetalResidentRawSource::retained_bytes() const noexcept {
    return 0U;
}

const RawBayerDenoiseReceipt& MetalResidentRawSource::raw_denoise_receipt() const noexcept {
    return implementation_->raw_denoise_receipt;
}

RawDemosaicReceipt MetalResidentRawSource::demosaic_receipt() const noexcept {
    return {};
}

bool MetalResidentRawSource::dcp_applied() const noexcept {
    return false;
}

bool MetalResidentRawSource::valid() const noexcept {
    return false;
}

void MetalResidentRawSource::invalidate() const noexcept {}

MetalResidentRawSourceTelemetry MetalResidentRawSource::telemetry() const noexcept {
    return {};
}

MetalResidentRawRegionLease
MetalResidentRawSource::develop_device_region(const GeometryPixelRect) const {
    throw DecodeError(
        DecodeErrorCode::unsupported,
        0,
        "Metal resident RAW source is unavailable on this platform"
    );
}

MetalResidentRawRegionLease
MetalResidentRawSource::develop_device_region(const GeometryPixelRect, const std::uint64_t) const {
    throw DecodeError(
        DecodeErrorCode::unsupported,
        0,
        "Metal resident RAW source is unavailable on this platform"
    );
}

DevelopedMetalResidentRawRegion
MetalResidentRawSource::develop_region(const GeometryPixelRect) const {
    throw DecodeError(
        DecodeErrorCode::unsupported,
        0,
        "Metal resident RAW source is unavailable on this platform"
    );
}

bool metal_resident_raw_source_available() noexcept {
    return false;
}

ResidentRawSourceAttempt try_prepare_metal_resident_raw_source(
    PreparedRawFrameSource prepared,
    detail::PreparedSceneLinearRegionOptics optics
) {
    if (!prepared.owns_region_optics(optics)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "prepared RAW source cannot publish with independently prepared region optics"
        );
    }
    if (prepared.development_.requested_backend() == RawDevelopmentBackendMode::cpu) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "a CPU-prepared RAW source cannot enter the Metal resident transaction"
        );
    }
    if (prepared.development_.requested_backend() == RawDevelopmentBackendMode::metal) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "Metal resident RAW source is unavailable on this platform"
        );
    }
    return ResidentRawSourceAttempt{
        .source = nullptr,
        .fallback_source = std::optional<PreparedRawFrameSource>(std::move(prepared)),
        .diagnostic = "Metal resident RAW source is unavailable on this platform",
    };
}

} // namespace shadow::image::raw_pipeline_detail
