#pragma once

#include <shadow/image/dcp_color_development.hpp>

#include "metal_raw_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shadow::image::detail {

struct MetalDcpHsvDelta final {
    float hue_shift_degrees = 0.0F;
    float saturation_scale = 1.0F;
    float value_scale = 1.0F;
};

struct MetalDcpToneCurvePoint final {
    float input = 0.0F;
    float output = 0.0F;
    float second_derivative = 0.0F;
};

struct MetalDcpPostParameters final {
    std::uint32_t pixel_count = 0U;
    std::uint32_t hue_hue_divisions = 0U;
    std::uint32_t hue_saturation_divisions = 0U;
    std::uint32_t hue_value_divisions = 0U;
    std::uint32_t hue_encoding_srgb = 0U;
    std::uint32_t look_hue_divisions = 0U;
    std::uint32_t look_saturation_divisions = 0U;
    std::uint32_t look_value_divisions = 0U;
    std::uint32_t look_encoding_srgb = 0U;
    std::uint32_t tone_curve_count = 0U;
    std::array<float, 9U> srgb_to_working{};
    std::array<float, 9U> working_to_srgb{};
};

static_assert(sizeof(MetalDcpHsvDelta) == 12U);
static_assert(sizeof(MetalDcpToneCurvePoint) == 12U);
static_assert(sizeof(MetalDcpPostParameters) == 112U);
static_assert(offsetof(MetalDcpPostParameters, pixel_count) == 0U);
static_assert(offsetof(MetalDcpPostParameters, tone_curve_count) == 36U);
static_assert(offsetof(MetalDcpPostParameters, srgb_to_working) == 40U);
static_assert(offsetof(MetalDcpPostParameters, working_to_srgb) == 76U);

// One immutable DCP table set can encode either a complete scene-linear frame or every output
// tile of a fused RAW reconstruction. It owns only the compact table buffers and never owns the
// large pixel buffer, command buffer, or backend-selection policy.
class MetalDcpColorEncoding final {
  public:
    MetalDcpColorEncoding(const MetalDcpColorEncoding&) = delete;
    MetalDcpColorEncoding& operator=(const MetalDcpColorEncoding&) = delete;

    ~MetalDcpColorEncoding();

    [[nodiscard]] static std::unique_ptr<MetalDcpColorEncoding> prepare(
        const DcpColorTransform& transform,
        std::uint32_t maximum_pixel_count,
        std::string& diagnostic
    );

    [[nodiscard]] std::size_t resource_bytes() const noexcept;
    [[nodiscard]] bool encode(
        id<MTLComputeCommandEncoder> encoder,
        id<MTLBuffer> pixels,
        std::uint32_t pixel_count,
        std::string& diagnostic
    ) const;

  private:
    MetalDcpColorEncoding() = default;

    MetalDcpPostParameters parameters_;
    id<MTLBuffer> hue_buffer_ = nil;
    id<MTLBuffer> look_buffer_ = nil;
    id<MTLBuffer> tone_buffer_ = nil;
    std::uint32_t maximum_pixel_count_ = 0U;
    std::size_t resource_bytes_ = 0U;
};

} // namespace shadow::image::detail
