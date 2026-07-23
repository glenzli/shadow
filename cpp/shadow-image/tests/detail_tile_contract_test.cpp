#include <shadow/image/edit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

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

class SyntheticDecodeSession final : public image::DecodeSession {
public:
    SyntheticDecodeSession(image::AssetMetadata metadata, image::PixelBuffer reference)
        : metadata_(std::move(metadata)), reference_(std::move(reference)) {
        capabilities_.metadata = true;
        capabilities_.reference_rgb = true;
    }

    [[nodiscard]] const image::AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const image::DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const image::PreviewDescriptor> previews() const noexcept override {
        return {};
    }

    [[nodiscard]] image::PreviewPayload decode_preview(std::size_t) override {
        throw image::DecodeError(
            image::DecodeErrorCode::no_preview,
            0,
            "synthetic detail fixture has no preview"
        );
    }

    [[nodiscard]] image::RawFrame decode_raw_frame() override {
        throw image::DecodeError(
            image::DecodeErrorCode::unsupported,
            0,
            "synthetic detail fixture has no RAW frame"
        );
    }

    [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
        ++render_count_;
        return reference_;
    }

    [[nodiscard]] int render_count() const noexcept {
        return render_count_;
    }

private:
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
    image::PixelBuffer reference_;
    mutable int render_count_ = 0;
};

[[nodiscard]] image::AssetMetadata metadata(const image::Dimensions dimensions) {
    image::AssetMetadata result;
    result.raw_dimensions = dimensions;
    result.image_dimensions = dimensions;
    return result;
}

[[nodiscard]] image::PixelBuffer reference_rgb(const image::Dimensions dimensions) {
    image::PixelBuffer result;
    result.dimensions = dimensions;
    result.bits_per_channel = 16;
    result.channels = 3;
    result.row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U
        * sizeof(std::uint16_t);
    result.primaries = image::RgbPrimaries::srgb_rec709_d65;
    result.transfer_function = image::RgbTransferFunction::linear;
    result.reference = image::RgbBufferReference::processed_raw;
    result.samples.resize(static_cast<std::size_t>(dimensions.pixel_count()) * 3U);
    for (std::uint32_t y = 0; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0; x < dimensions.width; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * dimensions.width + x) * 3U;
            result.samples[index] = x % 2U == 0U ? 0U : 65'535U;
            result.samples[index + 1U] = y % 2U == 0U ? 0U : 65'535U;
            result.samples[index + 2U] = (x + y) % 2U == 0U ? 0U : 65'535U;
        }
    }
    return result;
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

[[nodiscard]] std::array<image::AdjustmentNode, 1> neutral_plan() {
    return {
        image::AdjustmentNode{
            .node_id = "neutral-exposure",
            .parameters = image::ExposureAdjustment{},
        },
    };
}

template <typename Function>
void expect_decode_error(
    Function&& function,
    const image::DecodeErrorCode code,
    const std::string_view message
) {
    try {
        function();
        expect(false, message);
    } catch (const image::DecodeError& error) {
        expect(error.code() == code, message);
    }
}

void preparation_retains_one_immutable_source_and_tiles_exactly() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto source_bytes = static_cast<std::uint64_t>(dimensions.pixel_count()) * 3U
        * sizeof(std::uint16_t);
    const auto session = image::prepare_full_edit_detail(decoder);
    expect(decoder.render_count() == 1, "detail preparation renders the source exactly once");
    expect(session.dimensions() == dimensions, "detail session retains full dimensions");
    expect(
        session.retained_bytes() >= source_bytes
            && session.retained_bytes() <= image::maximum_full_edit_detail_retained_bytes,
        "detail session reports a bounded allocation covering every u16 sample"
    );

    const image::DetailTileRect rect{1, 1, 2, 2};
    const auto plan = neutral_plan();
    const auto first = session.render_rgb8(plan, rect);
    const auto second = session.render_rgb8(plan, rect);
    expect(decoder.render_count() == 1, "tile renders never ask the decoder to render again");
    expect(first.rect == rect, "tile result preserves its exact rectangle");
    expect(first.full_dimensions == dimensions, "tile result carries full dimensions");
    expect(first.row_stride_bytes == 6U, "RGB8 tile rows are tightly packed");
    expect(first.bytes == second.bytes, "identical detail renders are deterministic");
    expect(
        first.bytes[0] > first.bytes[2] && first.bytes[1] > first.bytes[2]
            && first.bytes[4] > first.bytes[3] && first.bytes[5] > first.bytes[3]
            && first.bytes[6] > first.bytes[7] && first.bytes[8] > first.bytes[7]
            && first.bytes[9] == 0U && first.bytes[10] == 0U && first.bytes[11] == 0U,
        "detail crop preserves full-resolution pixel coordinates and primary-color dominance"
    );
}

void adjustments_apply_only_to_the_requested_crop() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "black-lift",
            .parameters = image::OklabLightnessToneCurve{
                .lightness = image::ToneCurveSet{
                    .points = {{0.0, 0.25}, {1.0, 1.0}},
                },
            },
        },
    };
    const auto adjusted = session.render_rgb8(plan, {0, 0, 1, 1});
    expect(
        adjusted.bytes[0] > 0U && adjusted.bytes[0] == adjusted.bytes[1]
            && adjusted.bytes[1] == adjusted.bytes[2],
        "detail tile executes scene-linear nodes before the neutral display transform"
    );
    const auto neutral = session.render_rgb8(neutral_plan(), {0, 0, 1, 1});
    expect(
        neutral.bytes == std::vector<std::uint8_t>{0, 0, 0},
        "render-local edits never mutate the retained source"
    );
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

void neutral_scene_display_curve_retains_highlight_separation() {
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
        one_stop.bytes[0] < two_stops.bytes[0] && two_stops.bytes[0] < 255U
            && one_stop.bytes[0] == one_stop.bytes[1]
            && one_stop.bytes[1] == one_stop.bytes[2]
            && two_stops.bytes[0] == two_stops.bytes[1]
            && two_stops.bytes[1] == two_stops.bytes[2],
        "neutral display rendering rolls scene highlights into distinct SDR values"
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
    static_assert(image::display_srgb8_output_transform_version == 5U);
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

void irregular_tiles_match_one_full_pixel_local_execution_without_seams() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{.stops = -0.5},
        },
        image::AdjustmentNode{
            .node_id = "contrast",
            .parameters = image::ContrastAdjustment{.factor = 1.2, .pivot = 0.18},
        },
        image::AdjustmentNode{
            .node_id = "curve",
            .parameters = image::OklabLightnessToneCurve{
                .lightness = image::ToneCurveSet{
                    .points = {{0.0, 0.05}, {0.45, 0.3}, {1.0, 0.95}},
                },
            },
        },
        image::AdjustmentNode{
            .node_id = "white-balance",
            .parameters = image::RgbWhiteBalanceAdjustment{
                .temperature = 0.1,
                .tint = -0.05,
            },
        },
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.8},
        },
        image::AdjustmentNode{
            .node_id = "disabled",
            .enabled = false,
            .parameters = image::ExposureAdjustment{.stops = 4.0},
        },
    };
    const auto full = session.render_rgb8(plan, {0, 0, dimensions.width, dimensions.height});
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    for (const image::DetailTileRect rect : std::array{
             image::DetailTileRect{0, 0, 1, 3},
             image::DetailTileRect{1, 0, 3, 1},
             image::DetailTileRect{1, 1, 2, 2},
             image::DetailTileRect{3, 1, 1, 2},
         }) {
        const auto tile = session.render_rgb8(plan, rect);
        for (std::uint32_t row = 0; row < rect.height; ++row) {
            const auto source = tile.bytes.cbegin()
                + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(rect.y + row) * dimensions.width + rect.x) * 3U;
            std::copy_n(
                source,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "irregular tiles match one full execution across nonlinear and disabled nodes"
    );
}

void neighborhood_tiles_accumulate_two_sharpen_footprints_without_seams() {
    constexpr image::Dimensions dimensions{160, 50};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "wide-sharpen-first",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{
                .amount = 0.7,
                .radius = 5.0,
                .threshold = 0.05,
                .masking = 0.2,
            },
        },
        image::AdjustmentNode{
            .node_id = "wide-sharpen-second",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{
                .amount = 0.4,
                .radius = 5.0,
                .threshold = 0.1,
                .masking = 0.5,
            },
        },
    };
    const auto first_support = image::footprint(plan[0].parameters);
    const auto second_support = image::footprint(plan[1].parameters);
    expect(
        first_support.horizontal_radius + second_support.horizontal_radius == 30U,
        "two radius-five sharpen nodes require the sum of both 15-pixel supports"
    );

    const auto full = session.render_rgb8(plan, {0, 0, dimensions.width, dimensions.height});
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    constexpr std::array x_segments{
        std::pair<std::uint32_t, std::uint32_t>{0U, 37U},
        std::pair<std::uint32_t, std::uint32_t>{37U, 41U},
        std::pair<std::uint32_t, std::uint32_t>{78U, 82U},
    };
    constexpr std::array y_segments{
        std::pair<std::uint32_t, std::uint32_t>{0U, 19U},
        std::pair<std::uint32_t, std::uint32_t>{19U, 31U},
    };
    for (const auto [x, width] : x_segments) {
        for (const auto [y, height] : y_segments) {
            const image::DetailTileRect rect{x, y, width, height};
            const auto tile = session.render_rgb8(plan, rect);
            for (std::uint32_t row = 0U; row < height; ++row) {
                const auto source = tile.bytes.cbegin()
                    + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
                const std::size_t destination =
                    (static_cast<std::size_t>(y + row) * dimensions.width + x) * 3U;
                std::copy_n(
                    source,
                    static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                    stitched.begin() + static_cast<std::ptrdiff_t>(destination)
                );
            }
        }
    }
    expect(
        stitched == full.bytes,
        "two sequential neighborhood nodes render identically as full and irregular tiled images"
    );
}

void guided_selective_tone_tiles_match_full_execution_at_edges_and_boundaries() {
    constexpr image::Dimensions dimensions{224, 72};
    auto source = reference_rgb(dimensions);
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            // A broad dark region with gentle texture meets a three-stop highlight edge. This
            // exercises both the self-guided mask and a tile boundary that crosses the edge.
            const double base = x < 112U ? 0.045 + 0.004 * std::sin(
                static_cast<double>(x + y) * 0.18
            ) : 0.72 + 0.02 * std::cos(static_cast<double>(y) * 0.24);
            const auto encoded = static_cast<std::uint16_t>(std::clamp(
                std::llround(base * 65'535.0),
                0LL,
                65'535LL
            ));
            const std::size_t offset = (
                static_cast<std::size_t>(y) * dimensions.width + x
            ) * 3U;
            source.samples[offset] = encoded;
            source.samples[offset + 1U] = encoded;
            source.samples[offset + 2U] = encoded;
        }
    }
    SyntheticDecodeSession decoder(metadata(dimensions), std::move(source));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "guided-selective-tone",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{
                .highlights = -0.55,
                .shadows = 0.7,
                .whites = -0.15,
                .blacks = 0.2,
            },
        },
    };
    expect(
        image::footprint(plan[0].parameters).horizontal_radius
            == static_cast<std::uint32_t>(
                image::selective_tone_guided_mask_radius_level_zero
                    * image::selective_tone_guided_filter_box_passes
            ),
        "complete guided selective tone declares both box-pass supports for its detail-tile apron"
    );
    const auto full = session.render_rgb8(plan, {0, 0, dimensions.width, dimensions.height});
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    constexpr std::array tiles{
        image::DetailTileRect{0U, 0U, 73U, 29U},
        image::DetailTileRect{73U, 0U, 76U, 29U},
        image::DetailTileRect{149U, 0U, 75U, 29U},
        image::DetailTileRect{0U, 29U, 73U, 43U},
        image::DetailTileRect{73U, 29U, 76U, 43U},
        image::DetailTileRect{149U, 29U, 75U, 43U},
    };
    for (const image::DetailTileRect rect : tiles) {
        const auto tile = session.render_rgb8(plan, rect);
        for (std::uint32_t row = 0U; row < rect.height; ++row) {
            const auto begin = tile.bytes.cbegin()
                + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination = (
                static_cast<std::size_t>(rect.y + row) * dimensions.width + rect.x
            ) * 3U;
            std::copy_n(
                begin,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "guided selective tone produces identical full-frame and apron-expanded tiled cores"
    );
}

void neighborhood_resource_limits_fail_closed_before_allocation() {
    constexpr image::Dimensions dimensions{32, 32};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    std::vector<image::AdjustmentNode> plan;
    plan.reserve(35U);
    for (std::size_t index = 0U; index < 35U; ++index) {
        plan.push_back(image::AdjustmentNode{
            .node_id = "apron-limit-sharpen-" + std::to_string(index),
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{.amount = 1.0, .radius = 5.0},
        });
    }
    expect_decode_error(
        [&] { static_cast<void>(session.render_rgb8(plan, {0, 0, 1, 1})); },
        image::DecodeErrorCode::resource_limit,
        "a spatial plan exceeding the 512-pixel cumulative apron fails closed"
    );
}

void tile_shape_bounds_and_plan_fail_closed() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const auto plan = neutral_plan();
    for (const image::DetailTileRect rect : std::array{
             image::DetailTileRect{0, 0, 0, 1},
             image::DetailTileRect{0, 0, 1, 0},
             image::DetailTileRect{0, 0, image::maximum_edit_detail_tile_side + 1U, 1},
             image::DetailTileRect{4, 0, 1, 1},
             image::DetailTileRect{3, 0, 2, 1},
             image::DetailTileRect{0, 2, 1, 2},
             image::DetailTileRect{std::numeric_limits<std::uint32_t>::max(), 0, 1, 1},
         }) {
        expect_decode_error(
            [&] { static_cast<void>(session.render_rgb8(plan, rect)); },
            image::DecodeErrorCode::invalid_request,
            "invalid detail rectangle is rejected without unsigned overflow"
        );
    }

    const std::array invalid_plan{
        image::AdjustmentNode{
            .node_id = "invalid-exposure",
            .parameters = image::ExposureAdjustment{
                .stops = std::numeric_limits<double>::quiet_NaN(),
            },
        },
    };
    try {
        static_cast<void>(session.render_rgb8(invalid_plan, {0, 0, 0, 0}));
        expect(false, "invalid plan must fail before the invalid rectangle is evaluated");
    } catch (const image::EditError& error) {
        expect(
            error.code() == image::EditErrorCode::invalid_parameter,
            "detail render prevalidates the complete adjustment plan"
        );
    }
}

void metadata_limit_fails_before_reference_render() {
    constexpr image::Dimensions oversized{10'000, 10'000};
    constexpr image::Dimensions tiny{1, 1};
    SyntheticDecodeSession decoder(metadata(oversized), reference_rgb(tiny));
    expect_decode_error(
        [&] { static_cast<void>(image::prepare_full_edit_detail(decoder)); },
        image::DecodeErrorCode::resource_limit,
        "oversized metadata is rejected by the 512 MiB RGB u16 preflight"
    );
    expect(decoder.render_count() == 0, "metadata preflight runs before reference pixel I/O");
}

} // namespace

int main() {
    preparation_retains_one_immutable_source_and_tiles_exactly();
    adjustments_apply_only_to_the_requested_crop();
    processed_linear_grayscale_is_encoded_once_and_padding_is_ignored();
    neutral_scene_display_curve_retains_highlight_separation();
    display_quantization_dither_breaks_flat_8bit_contours_without_chroma_noise();
    processed_linear_contract_is_required_before_editing();
    display_gamut_mapping_preserves_oklab_hue_with_bounded_work();
    irregular_tiles_match_one_full_pixel_local_execution_without_seams();
    neighborhood_tiles_accumulate_two_sharpen_footprints_without_seams();
    guided_selective_tone_tiles_match_full_execution_at_edges_and_boundaries();
    neighborhood_resource_limits_fail_closed_before_allocation();
    tile_shape_bounds_and_plan_fail_closed();
    metadata_limit_fails_before_reference_render();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
