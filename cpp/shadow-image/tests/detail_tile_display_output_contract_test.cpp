#include "detail_tile_contract_test_support.hpp"

#include <shadow/image/full_edit_detail.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using shadow::image::test_support::SyntheticDecodeSession;
using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::neutral_plan;
using shadow::image::test_support::reference_rgb;

using shadow::image::test_support::expect_decode_error;

[[nodiscard]] double srgb8_to_linear(const std::uint8_t sample) {
    const double encoded = static_cast<double>(sample) / 255.0;
    return encoded <= 0.04045
        ? encoded / 12.92
        : std::pow((encoded + 0.055) / 1.055, 2.4);
}

[[nodiscard]] std::array<double, 3> linear_srgb_to_oklab(
    const std::array<double, 3>& rgb
) {
    const double l = std::cbrt(
        0.4122214708 * rgb[0] + 0.5363325363 * rgb[1] + 0.0514459929 * rgb[2]
    );
    const double m = std::cbrt(
        0.2119034982 * rgb[0] + 0.6806995451 * rgb[1] + 0.1073969566 * rgb[2]
    );
    const double s = std::cbrt(
        0.0883024619 * rgb[0] + 0.2817188376 * rgb[1] + 0.6299787005 * rgb[2]
    );
    return {
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

[[nodiscard]] std::array<double, 3> oklab_to_linear_srgb(
    const std::array<double, 3>& lab
) {
    const double l_root = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
    const double m_root = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
    const double s_root = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
    const double l = l_root * l_root * l_root;
    const double m = m_root * m_root * m_root;
    const double s = s_root * s_root * s_root;
    return {
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    };
}

[[nodiscard]] double oklab_hue_degrees(const std::array<double, 3>& rgb) {
    constexpr double radians_to_degrees = 57.2957795130823208768;
    const auto lab = linear_srgb_to_oklab(rgb);
    return std::atan2(lab[2], lab[1]) * radians_to_degrees;
}

[[nodiscard]] double circular_hue_distance(const double first, const double second) {
    return std::abs(std::remainder(first - second, 360.0));
}

[[nodiscard]] image::PixelBuffer grayscale_with_padding() {
    constexpr image::Dimensions dimensions{4, 2};
    constexpr std::size_t row_samples = 6;
    image::PixelBuffer result;
    result.dimensions = dimensions;
    result.bits_per_channel = 16;
    result.channels = 1;
    result.row_stride_bytes = row_samples * sizeof(std::uint16_t);
    result.primaries = image::RgbPrimaries::srgb_rec709_d65;
    result.transfer_function = image::RgbTransferFunction::linear;
    result.reference = image::RgbBufferReference::processed_raw;
    result.samples = {
        1, 257, 32'768, 65'534, 11'111, 22'222,
        65'534, 32'768, 257, 1, 33'333, 44'444,
    };
    return result;
}

void processed_linear_grayscale_is_encoded_once_and_padding_is_ignored() {
    constexpr image::Dimensions dimensions{4, 2};
    SyntheticDecodeSession decoder(metadata(dimensions), grayscale_with_padding());
    const auto session = image::prepare_full_edit_detail(decoder);
    const auto full = session.render_rgb8(
        neutral_plan(),
        {0, 0, dimensions.width, dimensions.height}
    );
    const auto gray = [&full](const std::size_t pixel) { return full.bytes[pixel * 3U]; };
    expect(
        gray(0U) == 0U && gray(0U) == full.bytes[1U] && gray(0U) == full.bytes[2U]
            && gray(0U) < gray(1U) && gray(1U) < gray(2U) && gray(2U) < gray(3U)
            && gray(4U) > gray(5U) && gray(5U) > gray(6U) && gray(6U) > gray(7U),
        "neutral detail applies a monotonic scene-to-display curve once to processed-linear grayscale"
    );
    const auto crop = session.render_rgb8(neutral_plan(), {1, 0, 2, 2});
    expect(
        crop.bytes == std::vector<std::uint8_t>{
            gray(1U), gray(1U), gray(1U),
            gray(2U), gray(2U), gray(2U),
            gray(5U), gray(5U), gray(5U),
            gray(6U), gray(6U), gray(6U),
        },
        "detail crop honors padded source rows without reading padding samples"
    );
}

void neutral_scene_display_curve_preserves_superwhite_order() {
    constexpr image::Dimensions dimensions{1, 1};
    auto source = reference_rgb(dimensions);
    source.samples = {32'768U, 32'768U, 32'768U};
    SyntheticDecodeSession decoder(metadata(dimensions), std::move(source));
    const auto session = image::prepare_full_edit_detail(decoder);
    const auto one_stop = session.render_rgb8(
        std::array{image::AdjustmentNode{
            .node_id = "one-stop",
            .parameters = image::ExposureAdjustment{.stops = 1.0},
        }},
        {0, 0, 1, 1}
    );
    const auto two_stops = session.render_rgb8(
        std::array{image::AdjustmentNode{
            .node_id = "two-stops",
            .parameters = image::ExposureAdjustment{.stops = 2.0},
        }},
        {0, 0, 1, 1}
    );
    expect(
        one_stop.bytes[0] >= 235U && one_stop.bytes[0] < 255U
            && two_stops.bytes[0] > one_stop.bytes[0] && two_stops.bytes[0] <= 255U
            && one_stop.bytes[0] == one_stop.bytes[1]
            && one_stop.bytes[1] == one_stop.bytes[2]
            && two_stops.bytes[0] == two_stops.bytes[1]
            && two_stops.bytes[1] == two_stops.bytes[2],
        "neutral display rendering keeps scene super-white ordered through the C1 SDR shoulder"
    );
}

void display_quantization_dither_breaks_flat_8bit_contours_without_chroma_noise() {
    constexpr image::Dimensions dimensions{16, 16};
    auto source = reference_rgb(dimensions);
    std::fill(source.samples.begin(), source.samples.end(), 16'384U);
    SyntheticDecodeSession decoder(metadata(dimensions), std::move(source));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array<image::AdjustmentNode, 0U> no_nodes{};
    const auto rendered = session.render_rgb8(
        no_nodes,
        image::DetailTileRect{.x = 0U, .y = 0U, .width = 16U, .height = 16U}
    );
    std::array<bool, 256U> observed{};
    for (std::size_t pixel = 0U; pixel < dimensions.pixel_count(); ++pixel) {
        const std::size_t offset = pixel * 3U;
        expect(
            rendered.bytes[offset] == rendered.bytes[offset + 1U]
                && rendered.bytes[offset + 1U] == rendered.bytes[offset + 2U],
            "display dither is shared across RGB channels and cannot introduce chroma speckle"
        );
        observed[rendered.bytes[offset]] = true;
    }
    const auto distinct = std::count(observed.begin(), observed.end(), true);
    expect(
        distinct >= 2,
        "display output distributes a flat intermediate tone across adjacent 8-bit codes"
    );
}

void processed_linear_contract_is_required_before_editing() {
    constexpr image::Dimensions dimensions{1, 1};
    auto invalid = reference_rgb(dimensions);
    invalid.transfer_function = image::RgbTransferFunction::unknown;
    SyntheticDecodeSession decoder(metadata(dimensions), std::move(invalid));
    expect_decode_error(
        [&decoder] { static_cast<void>(image::prepare_full_edit_detail(decoder)); },
        image::DecodeErrorCode::unsupported_layout,
        "an unspecified integer transfer function cannot enter the linear working pipeline"
    );
}

void display_gamut_mapping_preserves_oklab_hue_with_bounded_work() {
    static_assert(image::display_srgb8_output_transform_version == 3U);
    static_assert(image::display_srgb8_gamut_search_iterations <= 16U);
    static_assert(image::display_srgb8_maximum_oklab_chroma == 0.5);

    constexpr image::Dimensions dimensions{1, 1};
    auto source = reference_rgb(dimensions);
    source.samples = {32'768U, 16'384U, 8'192U};
    SyntheticDecodeSession decoder(metadata(dimensions), std::move(source));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "out-of-gamut-saturation",
            .parameters = image::SaturationAdjustment{.factor = 4.0},
        },
    };
    const auto rendered = session.render_rgb8(plan, {0, 0, 1, 1});
    expect(rendered.bytes.size() == 3U, "gamut-mapped detail returns one RGB8 pixel");

    constexpr double source_red = 32'768.0 / 65'535.0;
    constexpr double source_green = 16'384.0 / 65'535.0;
    constexpr double source_blue = 8'192.0 / 65'535.0;
    // Saturation is a perceptual Oklab chroma scale in the edit pipeline.  Construct the
    // pre-gamut expected hue from that same operation.  The neutral scene-display curve later
    // applies one common positive RGB gain, which leaves an Oklab hue unchanged; the former
    // linear-RGB/luma reference described an obsolete SaturationAdjustment implementation and
    // therefore made a correct hue-preserving map look like a regression.
    std::array<double, 3> saturated_lab = linear_srgb_to_oklab({
        source_red,
        source_green,
        source_blue,
    });
    saturated_lab[1] *= 4.0;
    saturated_lab[2] *= 4.0;
    const auto saturated_unclipped = oklab_to_linear_srgb(saturated_lab);
    const std::array<double, 3> mapped{
        srgb8_to_linear(rendered.bytes[0]),
        srgb8_to_linear(rendered.bytes[1]),
        srgb8_to_linear(rendered.bytes[2]),
    };
    const std::array<std::uint8_t, 3> independently_clipped{255U, 0U, 0U};
    expect(
        !std::equal(rendered.bytes.begin(), rendered.bytes.end(), independently_clipped.begin()),
        "display output desaturates along Oklab hue instead of clipping RGB independently"
    );
    expect(
        circular_hue_distance(
            oklab_hue_degrees(saturated_unclipped),
            oklab_hue_degrees(mapped)
        ) < 1.5,
        "8-bit gamut output preserves the saturated Oklab hue within quantization tolerance"
    );
}

} // namespace

int main() {
    processed_linear_grayscale_is_encoded_once_and_padding_is_ignored();
    neutral_scene_display_curve_preserves_superwhite_order();
    display_quantization_dither_breaks_flat_8bit_contours_without_chroma_noise();
    processed_linear_contract_is_required_before_editing();
    display_gamut_mapping_preserves_oklab_hue_with_bounded_work();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
