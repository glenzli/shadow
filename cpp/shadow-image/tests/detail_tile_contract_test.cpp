#include <shadow/image/edit.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
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

    [[nodiscard]] image::MosaicBuffer decode_mosaic() override {
        throw image::DecodeError(
            image::DecodeErrorCode::unsupported,
            0,
            "synthetic detail fixture has no mosaic"
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
    result.color_space = image::ColorSpace::srgb;
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
    result.color_space = image::ColorSpace::srgb;
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
        first.bytes == std::vector<std::uint8_t>{
            255, 255, 0,
            0, 255, 255,
            255, 0, 255,
            0, 0, 0,
        },
        "detail crop preserves exact full-resolution pixel coordinates"
    );
}

void adjustments_apply_only_to_the_requested_crop() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "black-lift",
            .parameters = image::ToneCurve{
                .points = {{0.0, 0.25}, {1.0, 1.0}},
            },
        },
    };
    const auto adjusted = session.render_rgb8(plan, {0, 0, 1, 1});
    expect(
        adjusted.bytes == std::vector<std::uint8_t>{137, 137, 137},
        "detail tile executes existing scene-linear nodes before sRGB8 output"
    );
    const auto neutral = session.render_rgb8(neutral_plan(), {0, 0, 1, 1});
    expect(
        neutral.bytes == std::vector<std::uint8_t>{0, 0, 0},
        "render-local edits never mutate the retained source"
    );
}

void grayscale_padding_and_midtones_match_the_existing_transfer_contract() {
    constexpr image::Dimensions dimensions{4, 2};
    SyntheticDecodeSession decoder(metadata(dimensions), grayscale_with_padding());
    const auto session = image::prepare_full_edit_detail(decoder);
    const auto full = session.render_rgb8(
        neutral_plan(),
        {0, 0, dimensions.width, dimensions.height}
    );
    expect(
        full.bytes == std::vector<std::uint8_t>{
            0, 0, 0,
            1, 1, 1,
            128, 128, 128,
            255, 255, 255,
            255, 255, 255,
            128, 128, 128,
            1, 1, 1,
            0, 0, 0,
        },
        "neutral detail preserves midtone sRGB quantization and replicates grayscale"
    );
    const auto crop = session.render_rgb8(neutral_plan(), {1, 0, 2, 2});
    expect(
        crop.bytes == std::vector<std::uint8_t>{
            1, 1, 1,
            128, 128, 128,
            128, 128, 128,
            1, 1, 1,
        },
        "detail crop honors padded source rows without reading padding samples"
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
            .parameters = image::ToneCurve{
                .points = {{0.0, 0.05}, {0.45, 0.3}, {1.0, 0.95}},
            },
        },
        image::AdjustmentNode{
            .node_id = "gain",
            .parameters = image::ChannelGainAdjustment{
                .channel_gains = {1.1, 0.95, 1.05},
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
    grayscale_padding_and_midtones_match_the_existing_transfer_contract();
    irregular_tiles_match_one_full_pixel_local_execution_without_seams();
    tile_shape_bounds_and_plan_fail_closed();
    metadata_limit_fails_before_reference_render();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
