#include "local_mask_coverage.hpp"
#include "condition_mask.hpp"

#include "managed_raster_mask.hpp"
#include "perceptual_hue_selection.hpp"
#include "photo_geometry_sampling.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace shadow::image::detail {

namespace {

[[nodiscard]] double smootherstep(const double value) noexcept {
    const double x = std::clamp(value, 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] double segment_distance(
    const double x,
    const double y,
    const LocalMaskPoint& start,
    const LocalMaskPoint& end,
    const double scale_x,
    const double scale_y
) noexcept {
    const double scaled_x = x * scale_x;
    const double scaled_y = y * scale_y;
    const double start_x = start.x * scale_x;
    const double start_y = start.y * scale_y;
    const double end_x = end.x * scale_x;
    const double end_y = end.y * scale_y;
    const double dx = end_x - start_x;
    const double dy = end_y - start_y;
    const double denominator = std::fma(dx, dx, dy * dy);
    if (denominator <= std::numeric_limits<double>::epsilon()) {
        return std::hypot(scaled_x - end_x, scaled_y - end_y);
    }
    const double projection =
        std::clamp(((scaled_x - start_x) * dx + (scaled_y - start_y) * dy) / denominator, 0.0, 1.0);
    return std::hypot(
        scaled_x - std::fma(projection, dx, start_x),
        scaled_y - std::fma(projection, dy, start_y)
    );
}

[[nodiscard]] double
luminance_range_coverage(const LocalMask& mask, const double lightness) noexcept {
    const double selected_lightness = std::clamp(lightness, 0.0, 1.0);
    if (mask.feather <= 0.0) {
        return selected_lightness >= mask.x0 && selected_lightness <= mask.x1 ? 1.0 : 0.0;
    }
    const double lower =
        smootherstep((selected_lightness - (mask.x0 - mask.feather)) / mask.feather);
    const double upper = 1.0 - smootherstep((selected_lightness - mask.x1) / mask.feather);
    return std::min(lower, upper);
}

} // namespace

PreparedLocalMaskCoverage prepare_local_mask_coverage(
    const LocalMask& mask,
    const FloatRgbImage& source,
    const Dimensions full_dimensions
) {
    const double shorter_side =
        static_cast<double>(std::min(full_dimensions.width, full_dimensions.height));
    PreparedLocalMaskCoverage prepared{
        .mask = &mask,
        .brush_scale_x = static_cast<double>(full_dimensions.width) / shorter_side,
        .brush_scale_y = static_cast<double>(full_dimensions.height) / shorter_side,
    };
    if (!mask.components.empty()) {
        prepared.components.reserve(mask.components.size());
        for (const LocalMaskComponent& component : mask.components) {
            prepared.components.push_back(
                prepare_local_mask_coverage(component.mask, source, full_dimensions)
            );
        }
    } else if (
        mask.kind == LocalMaskKind::luminance_range || mask.kind == LocalMaskKind::color_range
        || mask.kind == LocalMaskKind::condition_expression
    ) {
        prepared.color_transform = prepare_working_space_transform(source.working_space);
    } else if (
        mask.kind == LocalMaskKind::managed_raster && (mask.radius_y != 0.0 || mask.feather != 0.0)
    ) {
        prepared.refined_managed_raster =
            refine_managed_raster_mask(*mask.managed_raster, mask.radius_y, mask.feather);
    }
    return prepared;
}

double local_mask_coverage_at(
    const PreparedLocalMaskCoverage& prepared,
    const double normalized_x,
    const double normalized_y,
    const Vector3& source_rgb
) noexcept {
    const LocalMask& mask = *prepared.mask;
    if (!prepared.components.empty()) {
        double accumulated = 0.0;
        for (std::size_t index = 0U; index < mask.components.size(); ++index) {
            const LocalMaskComponent& component = mask.components[index];
            if (!component.enabled) {
                continue;
            }
            const double leaf = local_mask_coverage_at(
                prepared.components[index],
                normalized_x,
                normalized_y,
                source_rgb
            );
            switch (component.operation) {
            case LocalMaskComponentOperation::base:
                accumulated = leaf;
                break;
            case LocalMaskComponentOperation::add:
                accumulated = std::max(accumulated, leaf);
                break;
            case LocalMaskComponentOperation::subtract:
                accumulated = std::min(accumulated, 1.0 - leaf);
                break;
            case LocalMaskComponentOperation::intersect:
                accumulated = std::min(accumulated, leaf);
                break;
            }
        }
        if (mask.invert) {
            accumulated = 1.0 - accumulated;
        }
        return std::clamp(accumulated, 0.0, 1.0);
    }
    double coverage = 0.0;
    switch (mask.kind) {
    case LocalMaskKind::linear_gradient: {
        const double dx = mask.x1 - mask.x0;
        const double dy = mask.y1 - mask.y0;
        const double denominator = std::fma(dx, dx, dy * dy);
        coverage = std::clamp(
            ((normalized_x - mask.x0) * dx + (normalized_y - mask.y0) * dy) / denominator,
            0.0,
            1.0
        );
        break;
    }
    case LocalMaskKind::radial_gradient: {
        const double dx = (normalized_x - mask.x0) / mask.radius_x;
        const double dy = (normalized_y - mask.y0) / mask.radius_y;
        const double distance = std::sqrt(std::fma(dx, dx, dy * dy));
        if (mask.feather <= 0.0) {
            coverage = distance <= 1.0 ? 1.0 : 0.0;
        } else {
            const double inner = 1.0 - mask.feather;
            coverage = 1.0 - smootherstep((distance - inner) / mask.feather);
        }
        break;
    }
    case LocalMaskKind::brush: {
        if (mask.points.empty()) {
            coverage = 0.0;
            break;
        }
        double distance = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0U; index < mask.points.size(); ++index) {
            const auto& point = mask.points[index];
            distance = std::min(
                distance,
                std::hypot(
                    (normalized_x - point.x) * prepared.brush_scale_x,
                    (normalized_y - point.y) * prepared.brush_scale_y
                )
            );
            if (index > 0U && !point.begins_stroke) {
                distance = std::min(
                    distance,
                    segment_distance(
                        normalized_x,
                        normalized_y,
                        mask.points[index - 1U],
                        point,
                        prepared.brush_scale_x,
                        prepared.brush_scale_y
                    )
                );
            }
        }
        const double inner = mask.radius_x * (1.0 - mask.feather);
        const double transition =
            std::max(mask.radius_x - inner, std::numeric_limits<double>::epsilon());
        coverage = mask.feather <= 0.0 ? (distance <= mask.radius_x ? 1.0 : 0.0)
                                       : 1.0 - smootherstep((distance - inner) / transition);
        break;
    }
    case LocalMaskKind::luminance_range: {
        const Vector3 lab = working_rgb_to_oklab(*prepared.color_transform, source_rgb);
        coverage = luminance_range_coverage(mask, lab[0]);
        break;
    }
    case LocalMaskKind::color_range: {
        const Vector3 lab = working_rgb_to_oklab(*prepared.color_transform, source_rgb);
        const PerceptualHueSample hue = sample_oklab_hue(lab);
        coverage = hue.confidence
                   * perceptual_hue_range_weight(
                       hue.degrees,
                       mask.x0 * 360.0,
                       mask.x1 * 180.0,
                       mask.feather
                   );
        break;
    }
    case LocalMaskKind::condition_expression:
        coverage = condition_coverage(
            mask.condition_program,
            working_rgb_to_oklab(*prepared.color_transform, source_rgb)
        );
        break;
    case LocalMaskKind::managed_raster:
        coverage =
            prepared.refined_managed_raster.has_value()
                ? sample_refined_managed_raster_mask(
                      *prepared.refined_managed_raster,
                      normalized_x,
                      normalized_y
                  )
                : sample_managed_raster_mask(*mask.managed_raster, normalized_x, normalized_y);
        break;
    }
    if (mask.invert) {
        coverage = 1.0 - coverage;
    }
    return std::clamp(coverage, 0.0, 1.0);
}

bool LocalMaskCoverageRaster::valid() const noexcept {
    return dimensions.width > 0U && dimensions.height > 0U
           && dimensions.pixel_count() == static_cast<std::uint64_t>(samples.size());
}

bool LocalMaskCoverageR8::valid() const noexcept {
    return dimensions.width > 0U && dimensions.height > 0U && row_stride_bytes == dimensions.width
           && dimensions.pixel_count() == static_cast<std::uint64_t>(samples.size());
}

std::optional<LocalMaskCoverageRaster> render_local_mask_coverage(
    const FloatRgbImage& source,
    const LocalMask& mask,
    const AdjustmentExecutionContext context,
    const Dimensions full_dimensions,
    const std::stop_token cancellation,
    const std::optional<std::uint32_t> component_index
) {
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
    LocalMaskCoverageRaster output{
        .dimensions = source.dimensions,
        .samples =
            std::vector<float>(static_cast<std::size_t>(source.dimensions.pixel_count()), 0.0F),
    };
    const LocalMask* evaluated_mask = &mask;
    if (component_index.has_value()) {
        if (mask.components.empty()
            || static_cast<std::size_t>(*component_index) >= mask.components.size()) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "mask coverage component is outside the composite local mask"
            );
        }
        evaluated_mask = &mask.components[*component_index].mask;
    }
    const PreparedLocalMaskCoverage prepared =
        prepare_local_mask_coverage(*evaluated_mask, source, full_dimensions);
    const std::size_t source_stride = source.row_stride_bytes / sizeof(float);
    for (std::uint32_t row = 0U; row < source.dimensions.height; ++row) {
        if (cancellation.stop_requested()) {
            return std::nullopt;
        }
        const double normalized_y =
            (static_cast<double>(context.origin_y) + static_cast<double>(row) + 0.5)
            / static_cast<double>(full_dimensions.height);
        for (std::uint32_t column = 0U; column < source.dimensions.width; ++column) {
            const double normalized_x =
                (static_cast<double>(context.origin_x) + static_cast<double>(column) + 0.5)
                / static_cast<double>(full_dimensions.width);
            const std::size_t source_index = static_cast<std::size_t>(row) * source_stride
                                             + static_cast<std::size_t>(column) * 3U;
            const Vector3 source_rgb{
                static_cast<double>(source.samples[source_index]),
                static_cast<double>(source.samples[source_index + 1U]),
                static_cast<double>(source.samples[source_index + 2U]),
            };
            const std::size_t output_index =
                static_cast<std::size_t>(row) * source.dimensions.width + column;
            output.samples[output_index] = static_cast<float>(
                local_mask_coverage_at(prepared, normalized_x, normalized_y, source_rgb)
            );
        }
    }
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
    return output;
}

std::optional<LocalMaskCoverageR8> apply_local_mask_coverage_geometry(
    const LocalMaskCoverageRaster& source,
    const PhotoGeometry& geometry,
    const std::stop_token cancellation
) {
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
    if (!source.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "mask coverage geometry received an invalid scalar raster"
        );
    }
    const PhotoGeometryLayout layout = photo_geometry_layout(source.dimensions, geometry);
    LocalMaskCoverageR8 output{
        .dimensions = layout.output_dimensions,
        .row_stride_bytes = layout.output_dimensions.width,
        .samples = std::vector<std::uint8_t>(
            static_cast<std::size_t>(layout.output_dimensions.pixel_count())
        ),
    };
    const double crop_left = static_cast<double>(layout.source_crop.x);
    const double crop_top = static_cast<double>(layout.source_crop.y);
    const double crop_right =
        static_cast<double>(layout.source_crop.x + layout.source_crop.width - 1U);
    const double crop_bottom =
        static_cast<double>(layout.source_crop.y + layout.source_crop.height - 1U);
    for (std::uint32_t row = 0U; row < output.dimensions.height; ++row) {
        if (cancellation.stop_requested()) {
            return std::nullopt;
        }
        for (std::uint32_t column = 0U; column < output.dimensions.width; ++column) {
            const PhotoGeometrySourceCoordinate coordinate =
                photo_geometry_source_coordinate_for_output(layout, geometry, column, row);
            const std::size_t output_index =
                static_cast<std::size_t>(row) * output.dimensions.width + column;
            if (coordinate.x < crop_left || coordinate.x > crop_right || coordinate.y < crop_top
                || coordinate.y > crop_bottom) {
                output.samples[output_index] = 0U;
                continue;
            }
            const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(coordinate.x));
            const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(coordinate.y));
            const double fraction_x = coordinate.x - static_cast<double>(x0);
            const double fraction_y = coordinate.y - static_cast<double>(y0);
            const std::uint32_t x1 = std::min(
                fraction_x == 0.0 ? x0 : x0 + 1U,
                layout.source_crop.x + layout.source_crop.width - 1U
            );
            const std::uint32_t y1 = std::min(
                fraction_y == 0.0 ? y0 : y0 + 1U,
                layout.source_crop.y + layout.source_crop.height - 1U
            );
            const auto sample = [&](const std::uint32_t x, const std::uint32_t y) {
                return static_cast<double>(
                    source.samples[static_cast<std::size_t>(y) * source.dimensions.width + x]
                );
            };
            const double top = std::lerp(sample(x0, y0), sample(x1, y0), fraction_x);
            const double bottom = std::lerp(sample(x0, y1), sample(x1, y1), fraction_x);
            const double value = std::lerp(top, bottom, fraction_y);
            output.samples[output_index] =
                static_cast<std::uint8_t>(std::clamp(std::floor(value * 255.0 + 0.5), 0.0, 255.0));
        }
    }
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
    return output;
}

} // namespace shadow::image::detail
