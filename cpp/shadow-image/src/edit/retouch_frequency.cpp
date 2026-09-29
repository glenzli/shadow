#include "retouch_frequency.hpp"
#include "../concurrency/row_scheduler.hpp"
#include "scalar_neighborhood_filters.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <shadow/image/edit_error.hpp>
#include <vector>

namespace shadow::image::detail {
namespace {
double
sample(std::span<const double> field, std::size_t width, std::size_t height, double x, double y) {
    x = std::clamp(x, 0.0, double(width - 1));
    y = std::clamp(y, 0.0, double(height - 1));
    const auto x0 = std::size_t(std::floor(x)), y0 = std::size_t(std::floor(y));
    const auto x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    return std::lerp(
        std::lerp(field[y0 * width + x0], field[y0 * width + x1], x - double(x0)),
        std::lerp(field[y1 * width + x0], field[y1 * width + x1], x - double(x0)),
        y - double(y0)
    );
}
} // namespace
void apply_frequency_retouch(
    FloatRgbImage& image,
    const FloatRgbImage& source,
    const std::span<const float> coverage,
    const std::int64_t left,
    const std::int64_t top,
    const std::uint32_t width,
    const std::uint32_t height,
    const RetouchSourceMapping& mapping,
    const double strength,
    const bool texture,
    const std::uint16_t frequency_radius
) {
    if (width == 0 || height == 0 || strength <= 0)
        return;
    const double sx = frequency_radius * source.level_zero_to_raster_scale_x;
    const double sy = frequency_radius * source.level_zero_to_raster_scale_y;
    const auto halo_x = std::int64_t(std::max(1.0, std::ceil(3 * sx))) + 1;
    const auto halo_y = std::int64_t(std::max(1.0, std::ceil(3 * sy))) + 1;
    const auto right = left + width - 1, bottom = top + height - 1;
    double x0 = double(left), x1 = double(right), y0 = double(top), y1 = double(bottom);
    for (const auto x : {left, right})
        for (const auto y : {top, bottom}) {
            const double px = mapping.source_x(double(x), double(y));
            const double py = mapping.source_y(double(x), double(y));
            x0 = std::min(x0, px);
            x1 = std::max(x1, px);
            y0 = std::min(y0, py);
            y1 = std::max(y1, py);
        }
    const auto rx = std::max<std::int64_t>(0, std::int64_t(std::floor(x0)) - halo_x);
    const auto ry = std::max<std::int64_t>(0, std::int64_t(std::floor(y0)) - halo_y);
    const auto rr =
        std::min<std::int64_t>(source.dimensions.width - 1, std::int64_t(std::ceil(x1)) + halo_x);
    const auto rb =
        std::min<std::int64_t>(source.dimensions.height - 1, std::int64_t(std::ceil(y1)) + halo_y);
    const auto rw = std::size_t(rr - rx + 1), rh = std::size_t(rb - ry + 1);
    // Process channels sequentially: no full-frame high-frequency allocation,
    // and signed/HDR residuals are never clipped or converted to an RGB8 image.
    std::vector<double> field(rw * rh);
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t y = 0; y < rh; ++y) {
            throw_if_row_cancelled();
            for (std::size_t x = 0; x < rw; ++x)
                field[y * rw + x] =
                    source.samples
                        [((y + std::size_t(ry)) * source.dimensions.width + x + std::size_t(rx)) * 3
                         + c];
        }
        const auto low = gaussian_blur_scalar(field, rw, rh, sx, sy);
        for (std::uint32_t y = 0; y < height; ++y) {
            throw_if_row_cancelled();
            for (std::uint32_t x = 0; x < width; ++x) {
                const double alpha = strength * coverage[std::size_t(y) * width + x];
                if (alpha <= 0)
                    continue;
                const auto tx = left + x, ty = top + y;
                const double dx = mapping.source_x(double(tx), double(ty)) - double(rx);
                const double dy = mapping.source_y(double(tx), double(ty)) - double(ry);
                const auto i = std::size_t(ty - ry) * rw + std::size_t(tx - rx);
                const double low_delta = sample(low, rw, rh, dx, dy) - low[i];
                const double delta =
                    texture ? sample(field, rw, rh, dx, dy) - field[i] - low_delta : low_delta;
                const auto output =
                    (std::size_t(ty) * image.dimensions.width + std::size_t(tx)) * 3 + c;
                const double value = std::fma(alpha, delta, field[i]);
                if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                    throw EditError(
                        EditErrorCode::numeric_overflow,
                        std::nullopt,
                        "frequency repair produced a non-finite sample"
                    );
                image.samples[output] = static_cast<float>(value);
            }
        }
    }
}
} // namespace shadow::image::detail
