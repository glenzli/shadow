#include "metal_scene_linear_region_optics.hpp"

#include <shadow/image/decoder_error.hpp>

#include <utility>

namespace shadow::image::detail {

struct MetalSceneLinearRegionLease::Impl final {};

MetalSceneLinearRegionLease::MetalSceneLinearRegionLease(
    std::unique_ptr<Impl> implementation
) noexcept : implementation_(std::move(implementation)) {}

MetalSceneLinearRegionLease::MetalSceneLinearRegionLease(MetalSceneLinearRegionLease&&) noexcept =
    default;

MetalSceneLinearRegionLease&
MetalSceneLinearRegionLease::operator=(MetalSceneLinearRegionLease&&) noexcept = default;

MetalSceneLinearRegionLease::~MetalSceneLinearRegionLease() = default;

Dimensions MetalSceneLinearRegionLease::dimensions() const noexcept {
    return {};
}

std::uint64_t MetalSceneLinearRegionLease::device_identity() const noexcept {
    return 0U;
}

std::uint64_t MetalSceneLinearRegionLease::retained_bytes() const noexcept {
    return 0U;
}

std::uint64_t MetalSceneLinearRegionLease::completion_fence_value() const noexcept {
    return 0U;
}

bool MetalSceneLinearRegionLease::valid() const noexcept {
    return false;
}

MetalSceneLinearRegionOpticsTelemetry MetalSceneLinearRegionLease::telemetry() const noexcept {
    return {.invalidated = true};
}

SceneLinearRgbFrame MetalSceneLinearRegionLease::debug_readback() const {
    throw DecodeError(
        DecodeErrorCode::unsupported,
        0,
        "Metal scene-linear region optics is unavailable on this platform"
    );
}

void* MetalSceneLinearRegionLease::native_buffer_handle() const noexcept {
    return nullptr;
}

void* MetalSceneLinearRegionLease::native_device_handle() const noexcept {
    return nullptr;
}

void* MetalSceneLinearRegionLease::native_queue_handle() const noexcept {
    return nullptr;
}

MetalSceneLinearRegionLease develop_metal_scene_linear_region_optics(
    const raw_pipeline_detail::ResidentRawSource&,
    lensfun_modifier_plan::PreparedRegion,
    const std::uint64_t
) {
    throw DecodeError(
        DecodeErrorCode::unsupported,
        0,
        "Metal scene-linear region optics is unavailable on this platform"
    );
}

bool metal_scene_linear_region_optics_available() noexcept {
    return false;
}

const std::string& metal_scene_linear_region_optics_diagnostic() noexcept {
    static const std::string diagnostic =
        "Metal scene-linear region optics is unavailable on this platform";
    return diagnostic;
}

} // namespace shadow::image::detail
