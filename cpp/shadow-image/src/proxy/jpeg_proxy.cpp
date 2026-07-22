#include <shadow/image/edit.hpp>

#include "display_rgb_math.hpp"

#include <jpeglib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

struct JpegErrorManager final {
    jpeg_error_mgr base;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX]{};
    unsigned char* output = nullptr;
    unsigned long output_size = 0;
};

extern "C" void handle_jpeg_error(j_common_ptr context) {
    auto* error = reinterpret_cast<JpegErrorManager*>(context->err);
    (*context->err->format_message)(context, error->message);
    std::longjmp(error->jump, 1);
}

[[nodiscard]] std::size_t checked_rgb_size(const Dimensions dimensions) {
    const std::uint64_t pixels = dimensions.pixel_count();
    if (pixels > std::numeric_limits<std::uint64_t>::max() / 3U) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "proxy RGB sample count overflows"
        );
    }
    const std::uint64_t samples = pixels * 3U;
    if (samples > std::numeric_limits<std::size_t>::max()) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "proxy RGB buffer exceeds the address space"
        );
    }
    return static_cast<std::size_t>(samples);
}

void validate_jpeg_quality(const std::uint8_t jpeg_quality) {
    if (jpeg_quality == 0U || jpeg_quality > 100U) {
        throw DecodeError(DecodeErrorCode::invalid_request, 0, "JPEG quality must be in 1..=100");
    }
}

void validate_proxy_request(const ProxyRequest request) {
    constexpr std::uint32_t maximum_proxy_edge = 16'384;
    if (request.max_edge == 0U || request.max_edge > maximum_proxy_edge) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "proxy max edge must be in 1..=16384"
        );
    }
    validate_jpeg_quality(request.jpeg_quality);
}

void validate_warm_edit_max_edge(const std::uint32_t max_edge) {
    if (max_edge == 0U || max_edge > maximum_warm_edit_preview_edge) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "warm edit preview max edge must be in 1..=4096"
        );
    }
}

[[nodiscard]] std::size_t validated_source_row_stride(const PixelBuffer& source) {
    if (
        source.bits_per_channel != 16U || (source.channels != 1U && source.channels != 3U)
        || source.dimensions.width == 0U || source.dimensions.height == 0U
        || source.primaries != RgbPrimaries::srgb_rec709_d65
        || source.transfer_function != RgbTransferFunction::linear
        || source.reference != RgbBufferReference::processed_raw
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "proxy renderer requires non-empty 16-bit processed linear RGB with sRGB/Rec.709-D65 primaries"
        );
    }
    if (
        static_cast<std::uint64_t>(source.dimensions.width) * source.channels
        > std::numeric_limits<std::size_t>::max()
    ) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "proxy source stride overflows");
    }
    const std::size_t minimum_stride =
        static_cast<std::size_t>(source.dimensions.width) * source.channels;
    if (
        source.row_stride_bytes % sizeof(std::uint16_t) != 0U
        || source.row_stride_bytes / sizeof(std::uint16_t) < minimum_stride
    ) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "proxy source row stride is invalid");
    }
    const std::size_t row_stride = source.row_stride_bytes / sizeof(std::uint16_t);
    if (
        static_cast<std::uint64_t>(row_stride) * source.dimensions.height
        > std::numeric_limits<std::size_t>::max()
    ) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, "proxy source buffer overflows");
    }
    const std::size_t required_samples =
        row_stride * static_cast<std::size_t>(source.dimensions.height);
    if (source.samples.size() < required_samples) {
        throw DecodeError(DecodeErrorCode::corrupt_data, 0, "proxy source buffer is truncated");
    }
    return row_stride;
}

[[nodiscard]] std::uint16_t source_sample(
    const PixelBuffer& source,
    const std::size_t row_stride,
    const std::size_t x,
    const std::size_t y,
    const std::size_t channel
) {
    const std::size_t source_channel = source.channels == 1U ? 0U : channel;
    return source.samples[(y * row_stride) + (x * source.channels) + source_channel];
}

using LinearRgb = std::array<double, 3>;

struct OklabColor final {
    double lightness = 0.0;
    double a = 0.0;
    double b = 0.0;
};

[[nodiscard]] OklabColor linear_srgb_to_oklab(const LinearRgb& rgb) noexcept {
    const double l = std::cbrt(
        0.4122214708 * rgb[0] + 0.5363325363 * rgb[1] + 0.0514459929 * rgb[2]
    );
    const double m = std::cbrt(
        0.2119034982 * rgb[0] + 0.6806995451 * rgb[1] + 0.1073969566 * rgb[2]
    );
    const double s = std::cbrt(
        0.0883024619 * rgb[0] + 0.2817188376 * rgb[1] + 0.6299787005 * rgb[2]
    );
    return OklabColor{
        .lightness = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        .a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        .b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

[[nodiscard]] LinearRgb oklab_to_linear_srgb(const OklabColor& lab) noexcept {
    const double l_root = lab.lightness + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
    const double m_root = lab.lightness - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
    const double s_root = lab.lightness - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
    const double l = l_root * l_root * l_root;
    const double m = m_root * m_root * m_root;
    const double s = s_root * s_root * s_root;
    return {
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    };
}

[[nodiscard]] bool is_inside_display_srgb(const LinearRgb& rgb) noexcept {
    return std::ranges::all_of(rgb, [](const double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    });
}

[[nodiscard]] LinearRgb map_linear_srgb_to_display_gamut(const LinearRgb& input) {
    if (!std::ranges::all_of(input, [](const double value) { return std::isfinite(value); })) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited proxy contains a non-finite scene-linear sample"
        );
    }
    if (is_inside_display_srgb(input)) {
        return input;
    }

    // Output-transform v1 is deliberately small and deterministic: clamp only Oklab L to the
    // display interval, then binary-search chroma along the source hue ray. This prevents the
    // hue skews caused by independent RGB clipping. Values beyond display white/black still
    // collapse at the boundary; that is not, and must not be presented as, HDR tone mapping.
    OklabColor lab = linear_srgb_to_oklab(input);
    lab.lightness = std::clamp(lab.lightness, 0.0, 1.0);
    const double chroma = std::hypot(lab.a, lab.b);
    OklabColor neutral{.lightness = lab.lightness};
    LinearRgb best = oklab_to_linear_srgb(neutral);
    if (!std::isfinite(chroma) || chroma <= 1.0e-15) {
        for (double& channel : best) {
            channel = std::clamp(channel, 0.0, 1.0);
        }
        return best;
    }

    const double a_direction = lab.a / chroma;
    const double b_direction = lab.b / chroma;
    double lower = 0.0;
    // No display-sRGB hue/lightness slice reaches Oklab chroma 0.5. Limiting the bracket keeps
    // pathological scene values from expanding the hot-loop cost, while 16 steps yield a
    // sub-code-value chroma resolution for the final 8-bit target.
    double upper = std::min(chroma, display_srgb8_maximum_oklab_chroma);
    for (
        std::uint32_t iteration = 0U;
        iteration < display_srgb8_gamut_search_iterations;
        ++iteration
    ) {
        const double candidate_chroma = std::midpoint(lower, upper);
        const LinearRgb candidate = oklab_to_linear_srgb(OklabColor{
            .lightness = lab.lightness,
            .a = a_direction * candidate_chroma,
            .b = b_direction * candidate_chroma,
        });
        if (is_inside_display_srgb(candidate)) {
            lower = candidate_chroma;
            best = candidate;
        } else {
            upper = candidate_chroma;
        }
    }
    // Only absorb matrix round-off after the hue-preserving search; this is not the mapping.
    for (double& channel : best) {
        channel = std::clamp(channel, 0.0, 1.0);
    }
    return best;
}

[[nodiscard]] std::uint8_t linear_display_sample_to_srgb8(const double linear_sample) noexcept {
    const double linear = std::clamp(linear_sample, 0.0, 1.0);
    constexpr double srgb_linear_threshold = 0.0031308;
    const double encoded = linear <= srgb_linear_threshold
        ? 12.92 * linear
        : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(encoded * 255.0), 0L, 255L)
    );
}

[[nodiscard]] WorkingRgbSpace linear_srgb_working_space() {
    return WorkingRgbSpace{
        .id = "srgb-d65-linear",
        .primaries = {
            Chromaticity{0.6400, 0.3300},
            Chromaticity{0.3000, 0.6000},
            Chromaticity{0.1500, 0.0600},
        },
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] FloatRgbImage resize_processed_linear_to_working(
    const PixelBuffer& source,
    const Dimensions target
) {
    const std::size_t source_stride = validated_source_row_stride(source);
    const std::size_t sample_count = checked_rgb_size(target);
    const std::uint64_t row_samples = static_cast<std::uint64_t>(target.width) * 3U;
    if (row_samples > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "scene-linear proxy row stride overflows the address space"
        );
    }
    if (sample_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "scene-linear proxy buffer exceeds the address space"
        );
    }
    FloatRgbImage output;
    output.dimensions = target;
    output.row_stride_bytes = static_cast<std::size_t>(row_samples) * sizeof(float);
    output.pixel_format = FloatPixelFormat::rgb_f32_native_interleaved;
    output.transfer_function = TransferFunction::linear;
    // LibRaw's processed-linear result remains relative scene-referred after normalization:
    // it has no display OETF/look, but it is already WB/demosaiced/matrix-converted and is not
    // sensor-linear data. ImageReference::scene_referred intentionally carries that distinction.
    output.reference = ImageReference::scene_referred;
    output.working_space = linear_srgb_working_space();
    output.level_zero_to_raster_scale_x = static_cast<double>(target.width)
        / static_cast<double>(source.dimensions.width);
    output.level_zero_to_raster_scale_y = static_cast<double>(target.height)
        / static_cast<double>(source.dimensions.height);
    output.samples.resize(sample_count);

    const double scale_x =
        static_cast<double>(source.dimensions.width) / static_cast<double>(target.width);
    const double scale_y =
        static_cast<double>(source.dimensions.height) / static_cast<double>(target.height);

    for (std::uint32_t output_y = 0; output_y < target.height; ++output_y) {
        const double source_y =
            std::max(0.0, (static_cast<double>(output_y) + 0.5) * scale_y - 0.5);
        const auto y0 = static_cast<std::size_t>(source_y);
        const auto y1 = std::min(y0 + 1U, static_cast<std::size_t>(source.dimensions.height - 1U));
        const double fraction_y = source_y - static_cast<double>(y0);

        for (std::uint32_t output_x = 0; output_x < target.width; ++output_x) {
            const double source_x =
                std::max(0.0, (static_cast<double>(output_x) + 0.5) * scale_x - 0.5);
            const auto x0 = static_cast<std::size_t>(source_x);
            const auto x1 =
                std::min(x0 + 1U, static_cast<std::size_t>(source.dimensions.width - 1U));
            const double fraction_x = source_x - static_cast<double>(x0);
            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * target.width + output_x) * 3U;

            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const auto linear_sample = [&, channel](const std::size_t x, const std::size_t y) {
                    return static_cast<double>(
                        source_sample(source, source_stride, x, y, channel)
                    ) / 65'535.0;
                };
                const double top = linear_sample(x0, y0) * (1.0 - fraction_x)
                    + linear_sample(x1, y0) * fraction_x;
                const double bottom = linear_sample(x0, y1) * (1.0 - fraction_x)
                    + linear_sample(x1, y1) * fraction_x;
                output.samples[output_index + channel] =
                    static_cast<float>(top * (1.0 - fraction_y) + bottom * fraction_y);
            }
        }
    }
    return output;
}

[[nodiscard]] FloatRgbImage copy_processed_linear_to_working(const PixelBuffer& source) {
    return resize_processed_linear_to_working(source, source.dimensions);
}

void validate_detail_tile_rect(
    const DetailTileRect rect,
    const Dimensions full_dimensions
) {
    if (
        rect.width == 0U || rect.height == 0U
        || rect.width > maximum_edit_detail_tile_side
        || rect.height > maximum_edit_detail_tile_side
    ) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "detail tile width and height must be in 1..=1024"
        );
    }
    if (
        rect.x >= full_dimensions.width || rect.y >= full_dimensions.height
        || rect.width > full_dimensions.width - rect.x
        || rect.height > full_dimensions.height - rect.y
    ) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "detail tile rectangle must be fully inside the retained image"
        );
    }
}

[[nodiscard]] FloatRgbImage crop_processed_linear_to_working(
    const PixelBuffer& source,
    const DetailTileRect rect
) {
    const std::size_t source_stride = validated_source_row_stride(source);
    const Dimensions tile_dimensions{rect.width, rect.height};
    const std::size_t sample_count = checked_rgb_size(tile_dimensions);
    const std::uint64_t row_samples = static_cast<std::uint64_t>(rect.width) * 3U;
    if (row_samples > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "detail tile row stride overflows the address space"
        );
    }
    if (sample_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "detail tile float buffer exceeds the address space"
        );
    }

    FloatRgbImage output;
    output.dimensions = tile_dimensions;
    output.row_stride_bytes = static_cast<std::size_t>(row_samples) * sizeof(float);
    output.pixel_format = FloatPixelFormat::rgb_f32_native_interleaved;
    output.transfer_function = TransferFunction::linear;
    output.reference = ImageReference::scene_referred;
    output.working_space = linear_srgb_working_space();
    output.level_zero_to_raster_scale_x = 1.0;
    output.level_zero_to_raster_scale_y = 1.0;
    output.samples.resize(sample_count);

    for (std::uint32_t output_y = 0; output_y < rect.height; ++output_y) {
        const std::size_t source_y = static_cast<std::size_t>(rect.y) + output_y;
        for (std::uint32_t output_x = 0; output_x < rect.width; ++output_x) {
            const std::size_t source_x = static_cast<std::size_t>(rect.x) + output_x;
            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * rect.width + output_x) * 3U;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const double linear = static_cast<double>(
                    source_sample(source, source_stride, source_x, source_y, channel)
                ) / 65'535.0;
                output.samples[output_index + channel] =
                    static_cast<float>(linear);
            }
        }
    }
    return output;
}

[[nodiscard]] AdjustmentFootprint required_detail_apron(
    const std::span<const AdjustmentNode> nodes
) {
    std::uint64_t horizontal = 0U;
    std::uint64_t vertical = 0U;
    // Sequential neighborhood operations propagate boundary dependencies. Summing their
    // supports is the conservative reverse accumulation; taking only the maximum would create
    // seams as soon as two spatial nodes are enabled.
    for (const AdjustmentNode& node : nodes) {
        if (!node.enabled || locality(operation(node.parameters)) != AdjustmentLocality::neighborhood) {
            continue;
        }
        const AdjustmentFootprint node_footprint = footprint(node.parameters, 1.0, 1.0);
        horizontal += node_footprint.horizontal_radius;
        vertical += node_footprint.vertical_radius;
        if (
            horizontal > maximum_edit_detail_total_apron
            || vertical > maximum_edit_detail_total_apron
        ) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "detail adjustment apron exceeds the 512-pixel limit"
            );
        }
    }
    return AdjustmentFootprint{
        .horizontal_radius = static_cast<std::uint32_t>(horizontal),
        .vertical_radius = static_cast<std::uint32_t>(vertical),
    };
}

[[nodiscard]] DetailTileRect expanded_detail_rect(
    const DetailTileRect core,
    const Dimensions full_dimensions,
    const AdjustmentFootprint apron
) {
    const std::uint32_t left = std::min(core.x, apron.horizontal_radius);
    const std::uint32_t top = std::min(core.y, apron.vertical_radius);
    const std::uint32_t available_right = full_dimensions.width - (core.x + core.width);
    const std::uint32_t available_bottom = full_dimensions.height - (core.y + core.height);
    const std::uint32_t right = std::min(available_right, apron.horizontal_radius);
    const std::uint32_t bottom = std::min(available_bottom, apron.vertical_radius);
    const std::uint64_t width = static_cast<std::uint64_t>(core.width) + left + right;
    const std::uint64_t height = static_cast<std::uint64_t>(core.height) + top + bottom;
    if (
        width > maximum_edit_detail_working_side
        || height > maximum_edit_detail_working_side
        || width > std::numeric_limits<std::uint32_t>::max()
        || height > std::numeric_limits<std::uint32_t>::max()
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "expanded detail working region exceeds the 2048-pixel side limit"
        );
    }
    const std::uint64_t pixels = width * height;
    if (
        pixels > std::numeric_limits<std::size_t>::max() / 3U
        || pixels * 3U > std::numeric_limits<std::size_t>::max() / sizeof(float)
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "expanded detail working allocation exceeds the address space"
        );
    }
    return DetailTileRect{
        .x = core.x - left,
        .y = core.y - top,
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
    };
}

[[nodiscard]] FloatRgbImage crop_working_core(
    const FloatRgbImage& source,
    const std::uint32_t offset_x,
    const std::uint32_t offset_y,
    const Dimensions dimensions
) {
    if (
        offset_x > source.dimensions.width
        || offset_y > source.dimensions.height
        || dimensions.width > source.dimensions.width - offset_x
        || dimensions.height > source.dimensions.height - offset_y
    ) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "detail core lies outside its expanded working image"
        );
    }
    const std::size_t sample_count = checked_rgb_size(dimensions);
    const std::uint64_t row_samples = static_cast<std::uint64_t>(dimensions.width) * 3U;
    if (row_samples > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "detail core row stride exceeds the address space"
        );
    }

    FloatRgbImage output;
    output.dimensions = dimensions;
    output.row_stride_bytes = static_cast<std::size_t>(row_samples) * sizeof(float);
    output.pixel_format = source.pixel_format;
    output.transfer_function = source.transfer_function;
    output.reference = source.reference;
    output.working_space = source.working_space;
    output.level_zero_to_raster_scale_x = source.level_zero_to_raster_scale_x;
    output.level_zero_to_raster_scale_y = source.level_zero_to_raster_scale_y;
    output.samples.resize(sample_count);

    const std::size_t source_stride = source.row_stride_bytes / sizeof(float);
    const std::size_t output_stride = output.row_stride_bytes / sizeof(float);
    const std::size_t copy_samples = static_cast<std::size_t>(dimensions.width) * 3U;
    for (std::uint32_t row = 0U; row < dimensions.height; ++row) {
        const auto source_begin = source.samples.cbegin()
            + static_cast<std::ptrdiff_t>(
                (static_cast<std::size_t>(offset_y + row) * source_stride)
                + static_cast<std::size_t>(offset_x) * 3U
            );
        std::copy_n(
            source_begin,
            static_cast<std::ptrdiff_t>(copy_samples),
            output.samples.begin() + static_cast<std::ptrdiff_t>(
                static_cast<std::size_t>(row) * output_stride
            )
        );
    }
    return output;
}

[[nodiscard]] std::uint64_t checked_detail_retained_bytes(const PixelBuffer& source) {
    if (
        source.samples.capacity()
        > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail retained byte count overflows"
        );
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(source.samples.capacity()) * sizeof(std::uint16_t);
    if (bytes > maximum_full_edit_detail_retained_bytes) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail source exceeds the 512 MiB retained limit"
        );
    }
    return bytes;
}

void preflight_detail_metadata(const AssetMetadata& metadata) {
    const std::uint64_t pixels = std::max(
        metadata.raw_dimensions.pixel_count(),
        metadata.image_dimensions.pixel_count()
    );
    constexpr std::uint64_t rgb_u16_bytes_per_pixel = 3U * sizeof(std::uint16_t);
    if (
        pixels == 0U
        || pixels > maximum_full_edit_detail_retained_bytes / rgb_u16_bytes_per_pixel
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail metadata exceeds the 512 MiB worst-case RGB u16 limit"
        );
    }
}

void validate_display_output_source(const FloatRgbImage& source) {
    constexpr double coordinate_tolerance = 1.0e-9;
    const auto close = [](const double actual, const double expected) {
        return std::abs(actual - expected) <= coordinate_tolerance;
    };
    const bool is_linear_srgb =
        source.pixel_format == FloatPixelFormat::rgb_f32_native_interleaved
        && source.transfer_function == TransferFunction::linear
        && source.reference == ImageReference::scene_referred
        && close(source.working_space.primaries[0].x, 0.6400)
        && close(source.working_space.primaries[0].y, 0.3300)
        && close(source.working_space.primaries[1].x, 0.3000)
        && close(source.working_space.primaries[1].y, 0.6000)
        && close(source.working_space.primaries[2].x, 0.1500)
        && close(source.working_space.primaries[2].y, 0.0600)
        && close(source.working_space.white_point.x, 0.3127)
        && close(source.working_space.white_point.y, 0.3290);
    if (!is_linear_srgb) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "display output transform v1 requires scene-referred linear sRGB/Rec.709-D65 working RGB"
        );
    }
}

[[nodiscard]] std::vector<std::uint8_t> resize_working_to_display_srgb8(
    const FloatRgbImage& source,
    const Dimensions target
) {
    validate_display_output_source(source);
    std::vector<std::uint8_t> output(checked_rgb_size(target));
    const std::size_t row_stride = source.row_stride_bytes / sizeof(float);
    if (source.dimensions == target) {
        for (std::uint32_t y = 0; y < target.height; ++y) {
            const std::size_t source_row = static_cast<std::size_t>(y) * row_stride;
            const std::size_t output_row = static_cast<std::size_t>(y) * target.width * 3U;
            for (std::uint32_t x = 0; x < target.width; ++x) {
                const std::size_t source_index = source_row + static_cast<std::size_t>(x) * 3U;
                const std::size_t output_index = output_row + static_cast<std::size_t>(x) * 3U;
                const LinearRgb mapped = map_linear_srgb_to_display_gamut({
                    source.samples[source_index],
                    source.samples[source_index + 1U],
                    source.samples[source_index + 2U],
                });
                for (std::size_t channel = 0U; channel < mapped.size(); ++channel) {
                    output[output_index + channel] =
                        linear_display_sample_to_srgb8(mapped[channel]);
                }
            }
        }
        return output;
    }
    const double scale_x =
        static_cast<double>(source.dimensions.width) / static_cast<double>(target.width);
    const double scale_y =
        static_cast<double>(source.dimensions.height) / static_cast<double>(target.height);

    for (std::uint32_t output_y = 0; output_y < target.height; ++output_y) {
        const double source_y =
            std::max(0.0, (static_cast<double>(output_y) + 0.5) * scale_y - 0.5);
        const auto y0 = static_cast<std::size_t>(source_y);
        const auto y1 = std::min(y0 + 1U, static_cast<std::size_t>(source.dimensions.height - 1U));
        const double fraction_y = source_y - static_cast<double>(y0);

        for (std::uint32_t output_x = 0; output_x < target.width; ++output_x) {
            const double source_x =
                std::max(0.0, (static_cast<double>(output_x) + 0.5) * scale_x - 0.5);
            const auto x0 = static_cast<std::size_t>(source_x);
            const auto x1 =
                std::min(x0 + 1U, static_cast<std::size_t>(source.dimensions.width - 1U));
            const double fraction_x = source_x - static_cast<double>(x0);
            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * target.width + output_x) * 3U;

            LinearRgb linear{};
            for (std::size_t channel = 0; channel < linear.size(); ++channel) {
                const auto sample = [&, channel](const std::size_t x, const std::size_t y) {
                    return static_cast<double>(source.samples[(y * row_stride) + (x * 3U) + channel]);
                };
                const double top = sample(x0, y0) * (1.0 - fraction_x)
                    + sample(x1, y0) * fraction_x;
                const double bottom = sample(x0, y1) * (1.0 - fraction_x)
                    + sample(x1, y1) * fraction_x;
                linear[channel] = top * (1.0 - fraction_y) + bottom * fraction_y;
            }
            const LinearRgb mapped = map_linear_srgb_to_display_gamut(linear);
            for (std::size_t channel = 0U; channel < mapped.size(); ++channel) {
                output[output_index + channel] =
                    linear_display_sample_to_srgb8(mapped[channel]);
            }
        }
    }
    return output;
}

struct PreparedEditPreviewPixels final {
    FloatRgbImage edited;
    std::vector<std::uint8_t> rgb;
};

[[nodiscard]] PreparedEditPreviewPixels prepare_edit_preview_pixels(
    const FloatRgbImage& working_proxy,
    const std::span<const AdjustmentNode> nodes
) {
    FloatRgbImage edited = execute_adjustment_nodes(working_proxy, nodes);
    auto rgb = resize_working_to_display_srgb8(edited, edited.dimensions);
    return PreparedEditPreviewPixels{
        .edited = std::move(edited),
        .rgb = std::move(rgb),
    };
}

[[nodiscard]] EditPreviewAnalysis analyze_edit_preview(
    const FloatRgbImage& edited,
    const std::vector<std::uint8_t>& rgb
) {
    if (edited.dimensions.width == 0U || edited.dimensions.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited preview analysis received empty dimensions"
        );
    }
    const std::size_t expected_rgb_size = checked_rgb_size(edited.dimensions);
    const std::size_t minimum_row_samples =
        static_cast<std::size_t>(edited.dimensions.width) * 3U;
    if (edited.row_stride_bytes % sizeof(float) != 0U
        || edited.row_stride_bytes / sizeof(float) < minimum_row_samples
        || rgb.size() != expected_rgb_size) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited preview analysis received an invalid RGB layout"
        );
    }
    const std::size_t float_row_stride = edited.row_stride_bytes / sizeof(float);
    if (
        float_row_stride > std::numeric_limits<std::size_t>::max()
            / static_cast<std::size_t>(edited.dimensions.height)
    ) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited preview analysis scene-linear layout overflows"
        );
    }
    const std::size_t required_float_samples =
        float_row_stride * static_cast<std::size_t>(edited.dimensions.height);
    if (required_float_samples > edited.samples.size()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "edited preview analysis received truncated scene-linear pixels"
        );
    }

    EditPreviewAnalysis analysis;
    analysis.sample_dimensions = edited.dimensions;
    analysis.pixel_count = edited.dimensions.pixel_count();
    for (std::uint32_t y = 0; y < edited.dimensions.height; ++y) {
        for (std::uint32_t x = 0; x < edited.dimensions.width; ++x) {
            const std::size_t rgb_index =
                (static_cast<std::size_t>(y) * edited.dimensions.width + x) * 3U;
            const std::size_t float_index =
                static_cast<std::size_t>(y) * float_row_stride
                + static_cast<std::size_t>(x) * 3U;
            const std::uint8_t red = rgb[rgb_index];
            const std::uint8_t green = rgb[rgb_index + 1U];
            const std::uint8_t blue = rgb[rgb_index + 2U];
            ++analysis.red[red];
            ++analysis.green[green];
            ++analysis.blue[blue];
            ++analysis.luma[display_rgb::rec709_encoded_luma_u8(red, green, blue)];

            bool shadow_clipped = false;
            bool highlight_clipped = false;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const float sample = edited.samples[float_index + channel];
                if (sample < 0.0F) {
                    ++analysis.below_zero_samples[channel];
                    shadow_clipped = true;
                }
                if (sample > 1.0F) {
                    ++analysis.above_one_samples[channel];
                    highlight_clipped = true;
                }
            }
            analysis.shadow_clipped_pixels += shadow_clipped ? 1U : 0U;
            analysis.highlight_clipped_pixels += highlight_clipped ? 1U : 0U;
        }
    }
    return analysis;
}

[[nodiscard]] std::vector<std::uint8_t> encode_jpeg(
    const std::vector<std::uint8_t>& rgb,
    const Dimensions dimensions,
    const std::uint8_t quality
) {
    jpeg_compress_struct encoder{};
    JpegErrorManager error{};
    encoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = handle_jpeg_error;
    if (setjmp(error.jump) != 0) {
        std::free(error.output);
        jpeg_destroy_compress(&encoder);
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            std::string("JPEG proxy encoding failed: ") + error.message
        );
    }

    jpeg_create_compress(&encoder);
    jpeg_mem_dest(&encoder, &error.output, &error.output_size);
    encoder.image_width = dimensions.width;
    encoder.image_height = dimensions.height;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, quality, TRUE);
    encoder.optimize_coding = TRUE;
    jpeg_start_compress(&encoder, TRUE);

    const std::size_t row_stride = static_cast<std::size_t>(dimensions.width) * 3U;
    while (encoder.next_scanline < encoder.image_height) {
        auto* row = const_cast<JSAMPLE*>(
            rgb.data() + static_cast<std::size_t>(encoder.next_scanline) * row_stride
        );
        JSAMPROW rows[] = {row};
        jpeg_write_scanlines(&encoder, rows, 1);
    }
    jpeg_finish_compress(&encoder);

    std::vector<std::uint8_t> result(error.output, error.output + error.output_size);
    std::free(error.output);
    error.output = nullptr;
    jpeg_destroy_compress(&encoder);
    return result;
}

struct PreparedReferenceRgb final {
    PixelBuffer pixels;
    OpticsProfileReceipt optics_receipt;
};

[[nodiscard]] PreparedReferenceRgb prepare_reference_rgb(
    const DecodeSession& session,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    PixelBuffer pixels = session.render_reference_rgb();
    OpticsProfileReceipt receipt;
    if (optics_provider == nullptr) {
        receipt.status = OpticsProfileStatus::disabled;
        receipt.provider_id = "none";
        receipt.provider_version = "none";
        return {.pixels = std::move(pixels), .optics_receipt = std::move(receipt)};
    }
    auto corrected = optics_provider->correct_reference_rgb(
        pixels,
        session.metadata(),
        optics_settings
    );
    receipt = std::move(corrected.receipt);
    if (corrected.corrected_reference_rgb.has_value()) {
        pixels = std::move(*corrected.corrected_reference_rgb);
    }
    return {.pixels = std::move(pixels), .optics_receipt = std::move(receipt)};
}

} // namespace

WarmEditPreviewSession::WarmEditPreviewSession(
    FloatRgbImage working_proxy,
    const std::uint32_t max_edge,
    OpticsProfileReceipt optics_receipt
)
    : working_proxy_(std::move(working_proxy)), max_edge_(max_edge),
      optics_receipt_(std::move(optics_receipt)) {}

Dimensions WarmEditPreviewSession::dimensions() const noexcept {
    return working_proxy_.dimensions;
}

std::uint32_t WarmEditPreviewSession::max_edge() const noexcept {
    return max_edge_;
}

const OpticsProfileReceipt& WarmEditPreviewSession::optics_receipt() const noexcept {
    return optics_receipt_;
}

EncodedProxy WarmEditPreviewSession::render_jpeg(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality
) const {
    validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_pixels(working_proxy_, nodes);
    EncodedProxy proxy;
    proxy.dimensions = prepared.edited.dimensions;
    proxy.bytes = encode_jpeg(prepared.rgb, prepared.edited.dimensions, jpeg_quality);
    return proxy;
}

AnalyzedEditPreview WarmEditPreviewSession::render_jpeg_with_analysis(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality
) const {
    validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_pixels(working_proxy_, nodes);
    auto analysis = analyze_edit_preview(prepared.edited, prepared.rgb);

    EncodedProxy proxy;
    proxy.dimensions = prepared.edited.dimensions;
    proxy.bytes = encode_jpeg(
        prepared.rgb,
        prepared.edited.dimensions,
        jpeg_quality
    );
    return AnalyzedEditPreview{
        .proxy = std::move(proxy),
        .analysis = std::move(analysis),
    };
}

WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    validate_warm_edit_max_edge(max_edge);
    auto reference = prepare_reference_rgb(session, optics_provider, optics_settings);
    const Dimensions target = proxy_dimensions(reference.pixels.dimensions, max_edge);
    FloatRgbImage working_proxy = resize_processed_linear_to_working(reference.pixels, target);
    return WarmEditPreviewSession(
        std::move(working_proxy),
        max_edge,
        std::move(reference.optics_receipt)
    );
}

FullEditDetailSession::FullEditDetailSession(
    PixelBuffer reference_rgb,
    const std::uint64_t retained_bytes,
    OpticsProfileReceipt optics_receipt
)
    : reference_rgb_(std::move(reference_rgb)), retained_bytes_(retained_bytes),
      optics_receipt_(std::move(optics_receipt)) {}

Dimensions FullEditDetailSession::dimensions() const noexcept {
    return reference_rgb_.dimensions;
}

std::uint64_t FullEditDetailSession::retained_bytes() const noexcept {
    return retained_bytes_;
}

const OpticsProfileReceipt& FullEditDetailSession::optics_receipt() const noexcept {
    return optics_receipt_;
}

RenderedDetailTile FullEditDetailSession::render_rgb8(
    const std::span<const AdjustmentNode> nodes,
    const DetailTileRect rect
) const {
    validate_adjustment_nodes(nodes);
    validate_detail_tile_rect(rect, reference_rgb_.dimensions);
    const AdjustmentFootprint apron = required_detail_apron(nodes);
    const DetailTileRect working_rect = expanded_detail_rect(
        rect,
        reference_rgb_.dimensions,
        apron
    );
    const FloatRgbImage tile = crop_processed_linear_to_working(reference_rgb_, working_rect);
    const FloatRgbImage edited_working = execute_adjustment_nodes(
        tile,
        nodes,
        AdjustmentExecutionContext{
            .origin_x = working_rect.x,
            .origin_y = working_rect.y,
            .full_dimensions = reference_rgb_.dimensions,
        }
    );
    const Dimensions dimensions{rect.width, rect.height};
    const FloatRgbImage edited = crop_working_core(
        edited_working,
        rect.x - working_rect.x,
        rect.y - working_rect.y,
        dimensions
    );
    auto bytes = resize_working_to_display_srgb8(edited, dimensions);
    return RenderedDetailTile{
        .rect = rect,
        .full_dimensions = reference_rgb_.dimensions,
        .row_stride_bytes = rect.width * 3U,
        .bytes = std::move(bytes),
    };
}

FullEditDetailSession prepare_full_edit_detail(
    const DecodeSession& session,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    preflight_detail_metadata(session.metadata());
    auto reference = prepare_reference_rgb(session, optics_provider, optics_settings);
    static_cast<void>(validated_source_row_stride(reference.pixels));
    const std::uint64_t retained_bytes = checked_detail_retained_bytes(reference.pixels);
    return FullEditDetailSession(
        std::move(reference.pixels),
        retained_bytes,
        std::move(reference.optics_receipt)
    );
}

Dimensions proxy_dimensions(const Dimensions source, const std::uint32_t max_edge) {
    if (source.width == 0U || source.height == 0U || max_edge == 0U) {
        throw DecodeError(DecodeErrorCode::invalid_request, 0, "proxy dimensions must be non-zero");
    }
    const std::uint32_t source_edge = std::max(source.width, source.height);
    if (source_edge <= max_edge) {
        return source;
    }
    const auto scaled = [source_edge, max_edge](const std::uint32_t value) {
        const std::uint64_t numerator = static_cast<std::uint64_t>(value) * max_edge;
        return std::max(1U, static_cast<std::uint32_t>((numerator + source_edge / 2U) / source_edge));
    };
    return Dimensions{scaled(source.width), scaled(source.height)};
}

EncodedProxy render_reference_proxy_jpeg(const DecodeSession& session, const ProxyRequest request) {
    validate_proxy_request(request);
    const PixelBuffer source = session.render_reference_rgb();
    const Dimensions target = proxy_dimensions(source.dimensions, request.max_edge);
    const FloatRgbImage working = resize_processed_linear_to_working(source, target);
    const auto rgb = resize_working_to_display_srgb8(working, target);

    EncodedProxy proxy;
    proxy.dimensions = target;
    proxy.bytes = encode_jpeg(rgb, target, request.jpeg_quality);
    return proxy;
}

EncodedProxy render_edited_reference_proxy_jpeg(
    const DecodeSession& session,
    const std::span<const AdjustmentNode> nodes,
    const ProxyRequest request,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    validate_proxy_request(request);
    validate_adjustment_nodes(nodes);
    auto reference = prepare_reference_rgb(session, optics_provider, optics_settings);
    const FloatRgbImage scene_linear = copy_processed_linear_to_working(reference.pixels);
    const FloatRgbImage edited = execute_adjustment_nodes(scene_linear, nodes);
    const Dimensions target = proxy_dimensions(edited.dimensions, request.max_edge);
    const auto rgb = resize_working_to_display_srgb8(edited, target);

    EncodedProxy proxy;
    proxy.dimensions = target;
    proxy.bytes = encode_jpeg(rgb, target, request.jpeg_quality);
    return proxy;
}

} // namespace shadow::image
