#include "../edit/working_color_math.hpp"
#include "edit_preview_rendering.hpp"
#include "warm_edit_gpu.hpp"
#include <algorithm>
#include <cmath>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/warm_edit_preview.hpp>

namespace shadow::image {
CurveInputMap WarmEditPreviewSession::curve_input_map(
    std::span<const AdjustmentLayer> prefix,
    std::uint8_t channel,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify,
    std::stop_token cancellation
) const {
    if (channel > 4)
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "invalid curve input channel"
        );
    // This is the tool's explicit one-shot host consumer, never a per-pointer readback.
    auto prepared = edit_preview_detail::prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        prefix,
        geometry,
        liquify,
        sensor_clipping_mask_ ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_ ? &*highlight_chroma_risk_map_ : nullptr,
        true,
        cancellation,
        std::nullopt,
        detail::WarmEditGpuOutputIntent::host_rgb8
    );
    if (!prepared || !prepared->edited || cancellation.stop_requested())
        return {};
    const auto& input = *prepared->edited;
    const auto transform = detail::prepare_working_space_transform(input.working_space);
    const double scale =
        std::min(1.0, 512.0 / std::max(input.dimensions.width, input.dimensions.height));
    CurveInputMap map;
    map.dimensions = {
        std::max(1U, static_cast<std::uint32_t>(std::lround(input.dimensions.width * scale))),
        std::max(1U, static_cast<std::uint32_t>(std::lround(input.dimensions.height * scale)))
    };
    map.values.resize(map.dimensions.pixel_count());
    const auto stride = input.row_stride_bytes / sizeof(float);
    const auto encode = [](double value) {
        const double v = std::abs(value);
        return std::copysign(
            v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055,
            value
        );
    };
    for (std::uint32_t y = 0; y < map.dimensions.height; ++y) {
        if (cancellation.stop_requested())
            return {};
        for (std::uint32_t x = 0; x < map.dimensions.width; ++x) {
            const auto sx = std::min(
                input.dimensions.width - 1,
                static_cast<std::uint32_t>(
                    (x + 0.5) * input.dimensions.width / map.dimensions.width
                )
            );
            const auto sy = std::min(
                input.dimensions.height - 1,
                static_cast<std::uint32_t>(
                    (y + 0.5) * input.dimensions.height / map.dimensions.height
                )
            );
            detail::Vector3 rgb{};
            std::uint32_t count = 0;
            for (std::uint32_t yy = sy > 0 ? sy - 1 : 0;
                 yy <= std::min(sy + 1, input.dimensions.height - 1);
                 ++yy)
                for (std::uint32_t xx = sx > 0 ? sx - 1 : 0;
                     xx <= std::min(sx + 1, input.dimensions.width - 1);
                     ++xx) {
                    for (std::size_t c = 0; c < 3; ++c)
                        rgb[c] += input.samples[yy * stride + xx * 3U + c];
                    ++count;
                }
            for (auto& c : rgb)
                c /= count;
            // Master RGB uses an encoded-channel mean as its representative input tone.
            const double value = channel == 0 ? detail::working_rgb_to_oklab(transform, rgb)[0]
                                 : channel == 1
                                     ? (encode(rgb[0]) + encode(rgb[1]) + encode(rgb[2])) / 3.0
                                     : encode(rgb[channel - 2]);
            map.values[static_cast<std::size_t>(y) * map.dimensions.width + x] =
                static_cast<float>(value);
        }
    }
    return map;
}
} // namespace shadow::image
