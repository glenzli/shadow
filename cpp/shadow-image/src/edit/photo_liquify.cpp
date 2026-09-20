#include <shadow/image/decoder_error.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/working_rgb.hpp>

#include "photo_liquify_sampling.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace shadow::image {
namespace {

[[noreturn]] void invalid_liquify(const std::string_view detail) {
    throw DecodeError(
        DecodeErrorCode::invalid_request,
        0,
        "photo liquify " + std::string(detail)
    );
}

[[nodiscard]] bool normalized(const double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

[[nodiscard]] std::size_t rgb_sample_count(const Dimensions dimensions) {
    const std::uint64_t samples = static_cast<std::uint64_t>(dimensions.width)
        * static_cast<std::uint64_t>(dimensions.height) * 3U;
    if (samples == 0U || samples > std::numeric_limits<std::size_t>::max()) {
        invalid_liquify("dimensions overflow the RGB sample address space");
    }
    return static_cast<std::size_t>(samples);
}

void validate_image(const FloatRgbImage& image) {
    if (
        image.dimensions.width == 0U || image.dimensions.height == 0U
        || image.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || image.transfer_function != TransferFunction::linear
        || image.row_stride_bytes
            != static_cast<std::size_t>(image.dimensions.width) * 3U * sizeof(float)
        || image.samples.size() != rgb_sample_count(image.dimensions)
    ) {
        invalid_liquify("requires a contiguous linear RGB source raster");
    }
}

[[nodiscard]] double edge_coordinate(
    const double normalized_coordinate,
    const std::uint32_t extent
) noexcept {
    // Normalized authoring coordinates name image edges. Pixel centres live
    // at 0..extent-1, so the two outer edges are -0.5 and extent-0.5.
    return normalized_coordinate * static_cast<double>(extent) - 0.5;
}

} // namespace

PhotoLiquifyPoint photo_liquify_source_point(
    const PreparedPhotoLiquify& liquify,
    const PhotoLiquifyPoint output
) noexcept {
    if (liquify.source_dimensions.width == 0 || liquify.source_dimensions.height == 0)
        return output;
    const auto point = detail::inverse_photo_liquify_coordinate(
        liquify,
        output.x * liquify.source_dimensions.width - 0.5,
        output.y * liquify.source_dimensions.height - 0.5
    );
    return {
        (point.x + 0.5) / liquify.source_dimensions.width,
        (point.y + 0.5) / liquify.source_dimensions.height,
        output.pressure
    };
}

bool PreparedPhotoLiquify::valid() const noexcept {
    if (
        source_dimensions.width == 0U || source_dimensions.height == 0U || stamps.empty()
        || stamps.size() > maximum_prepared_photo_liquify_stamps
        || !std::isfinite(maximum_displacement_pixels)
        || maximum_displacement_pixels <= 0.0
    ) {
        return false;
    }
    double required_displacement_bound = 0.0;
    for (const auto& stamp : stamps) {
        if (!std::isfinite(stamp.center_x) || !std::isfinite(stamp.center_y)) {
            return false;
        }
        if (!std::isfinite(stamp.displacement_x)
            || !std::isfinite(stamp.displacement_y)
            || !std::isfinite(stamp.reconstruction) || !std::isfinite(stamp.radius)
            || stamp.radius <= 0.0 || !normalized(stamp.hardness)) {
            return false;
        }
        switch (stamp.kind) {
        case PreparedPhotoLiquifyStampKind::push:
            if (stamp.reconstruction != 0.0
                || (stamp.displacement_x == 0.0 && stamp.displacement_y == 0.0)) {
                return false;
            }
            required_displacement_bound +=
                std::hypot(stamp.displacement_x, stamp.displacement_y);
            break;
        case PreparedPhotoLiquifyStampKind::reconstruct:
            if (stamp.displacement_x != 0.0 || stamp.displacement_y != 0.0
                || stamp.reconstruction <= 0.0 || stamp.reconstruction > 1.0) {
                return false;
            }
            break;
        }
    }
    return std::isfinite(required_displacement_bound)
        && maximum_displacement_pixels >= required_displacement_bound;
}

void validate_photo_liquify(const PhotoLiquify& liquify) {
    if (liquify.strokes.empty()) {
        invalid_liquify("node must contain at least one gesture");
    }
    if (liquify.strokes.size() > maximum_photo_liquify_strokes) {
        invalid_liquify("node exceeds the supported gesture bound");
    }
    bool has_prior_deformation = false;
    for (const auto& operation : liquify.strokes) {
        const auto& points = std::visit([](const auto& stroke) -> const auto& {
            return stroke.points;
        }, operation);
        const double radius =
            std::visit([](const auto& stroke) { return stroke.radius; }, operation);
        const double strength =
            std::visit([](const auto& stroke) { return stroke.strength; }, operation);
        const double hardness =
            std::visit([](const auto& stroke) { return stroke.hardness; }, operation);
        const bool reconstruct =
            std::holds_alternative<PhotoLiquifyReconstructStroke>(operation);
        if (reconstruct && !has_prior_deformation) {
            invalid_liquify("reconstruct gesture requires earlier deformation");
        }
        if ((!reconstruct && points.size() < 2U) || (reconstruct && points.empty())) {
            invalid_liquify(
                reconstruct ? "reconstruct gesture must contain at least one sample"
                            : "push gesture must contain at least two samples"
            );
        }
        if (points.size() > maximum_photo_liquify_points_per_stroke) {
            invalid_liquify("gesture exceeds the supported sample bound");
        }
        if (!normalized(radius) || radius == 0.0) {
            invalid_liquify("brush radius must be normalized and greater than zero");
        }
        if (!normalized(strength) || strength == 0.0) {
            invalid_liquify("brush strength must be normalized and greater than zero");
        }
        if (!normalized(hardness)) {
            invalid_liquify("brush hardness must be normalized");
        }
        for (const auto& point : points) {
            if (!normalized(point.x) || !normalized(point.y) || !normalized(point.pressure)) {
                invalid_liquify("gesture samples must contain normalized finite values");
            }
        }
        if (reconstruct) {
            if (std::none_of(points.begin(), points.end(), [](const PhotoLiquifyPoint& point) {
                    return point.pressure > 0.0;
                })) {
                invalid_liquify("reconstruct gesture must contain effective pressure");
            }
        } else {
            has_prior_deformation = true;
        }
    }
}

PreparedPhotoLiquify prepare_photo_liquify(
    const Dimensions source_dimensions,
    const PhotoLiquify& liquify
) {
    validate_photo_liquify(liquify);
    if (source_dimensions.width == 0U || source_dimensions.height == 0U) {
        invalid_liquify("preparation requires non-zero source dimensions");
    }

    PreparedPhotoLiquify prepared{
        .source_dimensions = source_dimensions,
    };
    const double shorter_edge = static_cast<double>(
        std::min(source_dimensions.width, source_dimensions.height)
    );
    for (const auto& operation : liquify.strokes) {
        if (const auto* const reconstruct =
                std::get_if<PhotoLiquifyReconstructStroke>(&operation);
            reconstruct != nullptr) {
            const double radius = std::max(0.5, reconstruct->radius * shorter_edge);
            const double maximum_spacing = std::max(0.5, radius * 0.25);
            const auto append_reconstruct_stamp =
                [&](const PhotoLiquifyPoint& point, const double amount) {
                    if (amount <= 0.0) {
                        return;
                    }
                    if (prepared.stamps.size() >= maximum_prepared_photo_liquify_stamps) {
                        invalid_liquify(
                            "prepared stamp count exceeds the fixed execution bound"
                        );
                    }
                    prepared.stamps.push_back(PreparedPhotoLiquifyStamp{
                        .kind = PreparedPhotoLiquifyStampKind::reconstruct,
                        .center_x = edge_coordinate(point.x, source_dimensions.width),
                        .center_y = edge_coordinate(point.y, source_dimensions.height),
                        .reconstruction = amount,
                        .radius = radius,
                        .hardness = reconstruct->hardness,
                    });
                };
            append_reconstruct_stamp(
                reconstruct->points.front(),
                reconstruct->strength * reconstruct->points.front().pressure
            );
            for (std::size_t point_index = 1U; point_index < reconstruct->points.size();
                 ++point_index) {
                const auto& from = reconstruct->points[point_index - 1U];
                const auto& to = reconstruct->points[point_index];
                const double from_x = edge_coordinate(from.x, source_dimensions.width);
                const double from_y = edge_coordinate(from.y, source_dimensions.height);
                const double delta_x =
                    edge_coordinate(to.x, source_dimensions.width) - from_x;
                const double delta_y =
                    edge_coordinate(to.y, source_dimensions.height) - from_y;
                const double length = std::hypot(delta_x, delta_y);
                if (length == 0.0) {
                    continue;
                }
                const std::size_t step_count = static_cast<std::size_t>(
                    std::max(1.0, std::ceil(length / maximum_spacing))
                );
                if (step_count > maximum_prepared_photo_liquify_stamps - prepared.stamps.size()) {
                    invalid_liquify("prepared stamp count exceeds the fixed execution bound");
                }
                const double inverse_step_count = 1.0 / static_cast<double>(step_count);
                for (std::size_t step = 1U; step <= step_count; ++step) {
                    const double interpolation =
                        static_cast<double>(step) * inverse_step_count;
                    const double pressure =
                        from.pressure + (to.pressure - from.pressure) * interpolation;
                    const double segment_amount =
                        std::clamp(reconstruct->strength * pressure, 0.0, 1.0);
                    const double amount = 1.0
                        - std::pow(1.0 - segment_amount, inverse_step_count);
                    append_reconstruct_stamp(
                        PhotoLiquifyPoint{
                            .x = from.x + (to.x - from.x) * interpolation,
                            .y = from.y + (to.y - from.y) * interpolation,
                            .pressure = pressure,
                        },
                        amount
                    );
                }
            }
            continue;
        }
        const auto& stroke = std::get<PhotoLiquifyPushStroke>(operation);
        const double radius = std::max(0.5, stroke.radius * shorter_edge);
        // A quarter-radius upper spacing keeps neighbouring radial supports
        // overlapping while a half-pixel floor bounds very small proxy work.
        const double maximum_spacing = std::max(0.5, radius * 0.25);
        for (std::size_t point_index = 1U; point_index < stroke.points.size(); ++point_index) {
            const auto& from = stroke.points[point_index - 1U];
            const auto& to = stroke.points[point_index];
            const double from_x = edge_coordinate(from.x, source_dimensions.width);
            const double from_y = edge_coordinate(from.y, source_dimensions.height);
            const double delta_x = edge_coordinate(to.x, source_dimensions.width) - from_x;
            const double delta_y = edge_coordinate(to.y, source_dimensions.height) - from_y;
            const double length = std::hypot(delta_x, delta_y);
            if (length == 0.0) {
                continue;
            }
            const double step_count_f64 = std::max(1.0, std::ceil(length / maximum_spacing));
            if (
                step_count_f64
                > static_cast<double>(
                    maximum_prepared_photo_liquify_stamps - prepared.stamps.size()
                )
            ) {
                invalid_liquify("prepared stamp count exceeds the fixed execution bound");
            }
            const auto step_count = static_cast<std::size_t>(step_count_f64);
            const double inverse_step_count = 1.0 / step_count_f64;
            for (std::size_t step = 1U; step <= step_count; ++step) {
                const double interpolation = static_cast<double>(step) * inverse_step_count;
                const double pressure =
                    from.pressure + (to.pressure - from.pressure) * interpolation;
                const double displacement_scale =
                    inverse_step_count * stroke.strength * pressure;
                const double displacement_x = delta_x * displacement_scale;
                const double displacement_y = delta_y * displacement_scale;
                if (displacement_x == 0.0 && displacement_y == 0.0) {
                    continue;
                }
                prepared.stamps.push_back(PreparedPhotoLiquifyStamp{
                    .kind = PreparedPhotoLiquifyStampKind::push,
                    .center_x = from_x + delta_x * interpolation,
                    .center_y = from_y + delta_y * interpolation,
                    .displacement_x = displacement_x,
                    .displacement_y = displacement_y,
                    .radius = radius,
                    .hardness = stroke.hardness,
                });
                prepared.maximum_displacement_pixels +=
                    std::hypot(displacement_x, displacement_y);
            }
        }
    }
    if (!prepared.valid()) {
        invalid_liquify("authored gestures produce no effective displacement");
    }
    return prepared;
}

FloatRgbImage apply_photo_liquify(
    const FloatRgbImage& source,
    const PreparedPhotoLiquify& liquify
) {
    validate_image(source);
    if (!liquify.valid() || liquify.source_dimensions != source.dimensions) {
        invalid_liquify("prepared execution does not match the source raster");
    }

    FloatRgbImage output = source;
    output.samples.resize(rgb_sample_count(source.dimensions));
    const double maximum_x = static_cast<double>(source.dimensions.width - 1U);
    const double maximum_y = static_cast<double>(source.dimensions.height - 1U);
    for (std::uint32_t y = 0U; y < source.dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < source.dimensions.width; ++x) {
            // Gestures were authored forward in time. Inverse sampling walks
            // them backward so all deformation is composed before one RGB
            // interpolation, avoiding quality loss from per-stroke resampling.
            const auto coordinate = detail::inverse_photo_liquify_coordinate(
                liquify,
                static_cast<double>(x),
                static_cast<double>(y)
            );
            double source_x = coordinate.x;
            double source_y = coordinate.y;
            source_x = std::clamp(source_x, 0.0, maximum_x);
            source_y = std::clamp(source_y, 0.0, maximum_y);
            const auto x0 = static_cast<std::uint32_t>(std::floor(source_x));
            const auto y0 = static_cast<std::uint32_t>(std::floor(source_y));
            const std::uint32_t x1 = std::min(x0 + 1U, source.dimensions.width - 1U);
            const std::uint32_t y1 = std::min(y0 + 1U, source.dimensions.height - 1U);
            const double fraction_x = source_x - static_cast<double>(x0);
            const double fraction_y = source_y - static_cast<double>(y0);
            const auto sample_index = [&](const std::uint32_t sample_x,
                                          const std::uint32_t sample_y) {
                return (
                    static_cast<std::size_t>(sample_y) * source.dimensions.width + sample_x
                ) * 3U;
            };
            const std::size_t index_00 = sample_index(x0, y0);
            const std::size_t index_10 = sample_index(x1, y0);
            const std::size_t index_01 = sample_index(x0, y1);
            const std::size_t index_11 = sample_index(x1, y1);
            const std::size_t output_index = sample_index(x, y);
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const double top = static_cast<double>(source.samples[index_00 + channel])
                    + (
                        static_cast<double>(source.samples[index_10 + channel])
                        - static_cast<double>(source.samples[index_00 + channel])
                    ) * fraction_x;
                const double bottom = static_cast<double>(source.samples[index_01 + channel])
                    + (
                        static_cast<double>(source.samples[index_11 + channel])
                        - static_cast<double>(source.samples[index_01 + channel])
                    ) * fraction_x;
                output.samples[output_index + channel] =
                    static_cast<float>(top + (bottom - top) * fraction_y);
            }
        }
    }
    return output;
}

} // namespace shadow::image
