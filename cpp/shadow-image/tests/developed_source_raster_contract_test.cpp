#include "developed_source_raster.hpp"

#include <shadow/image/decoder_error.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace image = shadow::image;
namespace proxy_detail = shadow::image::proxy_detail;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void expect_near(const float actual, const float expected, const std::string_view message) {
    expect(std::abs(actual - expected) < 1.0e-6F, message);
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

[[nodiscard]] image::DevelopedSourcePixels padded_grayscale_source() {
    image::PixelBuffer source;
    source.dimensions = {3U, 2U};
    source.bits_per_channel = 16U;
    source.channels = 1U;
    source.row_stride_bytes = 4U * sizeof(std::uint16_t);
    source.primaries = image::RgbPrimaries::srgb_rec709_d65;
    source.transfer_function = image::RgbTransferFunction::linear;
    source.reference = image::RgbBufferReference::processed_raw;
    source.samples = {
        0U,
        32'768U,
        65'535U,
        7'777U,
        65'535U,
        32'768U,
        0U,
        8'888U,
    };
    return source;
}

void pixel_buffer_dispatch_preserves_padding_and_grayscale() {
    const image::DevelopedSourcePixels source = padded_grayscale_source();
    const image::FloatRgbImage crop = proxy_detail::crop_developed_source_to_working(
        source,
        image::GeometryPixelRect{.x = 1U, .y = 0U, .width = 2U, .height = 2U}
    );

    expect(
        crop.dimensions == image::Dimensions{2U, 2U},
        "developed source crop preserves requested dimensions"
    );
    expect(
        crop.reference == image::ImageReference::scene_referred,
        "processed RAW crop remains scene-referred"
    );
    expect(crop.samples.size() == 12U, "grayscale crop expands to interleaved RGB");
    const float half = 32'768.0F / 65'535.0F;
    const std::vector<float> expected = {
        half,
        half,
        half,
        1.0F,
        1.0F,
        1.0F,
        half,
        half,
        half,
        0.0F,
        0.0F,
        0.0F,
    };
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        expect_near(crop.samples[index], expected[index], "crop ignores padded source samples");
    }

    const image::FloatRgbImage resized =
        proxy_detail::resize_developed_source_to_working(source, image::Dimensions{1U, 1U});
    expect(resized.samples.size() == 3U, "resize emits one interleaved RGB pixel");
    for (const float sample : resized.samples) {
        expect_near(sample, half, "resize dispatch retains the original bilinear sampling");
    }
    expect_near(
        static_cast<float>(resized.level_zero_to_raster_scale_x),
        1.0F / 3.0F,
        "resize records horizontal level-zero scale"
    );
    expect_near(
        static_cast<float>(resized.level_zero_to_raster_scale_y),
        0.5F,
        "resize records vertical level-zero scale"
    );
}

void scene_linear_dispatch_preserves_unbounded_samples() {
    image::SceneLinearRgbFrame frame{
        .dimensions = {2U, 1U},
        .row_stride_bytes = 6U * sizeof(float),
        .samples = {-0.25F, 0.5F, 1.5F, 2.0F, -0.5F, 0.125F},
    };
    const image::DevelopedSourcePixels source{std::move(frame)};
    const image::FloatRgbImage crop = proxy_detail::crop_developed_source_to_working(
        source,
        image::GeometryPixelRect{.x = 1U, .y = 0U, .width = 1U, .height = 1U}
    );

    expect(crop.samples.size() == 3U, "scene-linear crop emits one RGB pixel");
    expect_near(crop.samples[0], 2.0F, "scene-linear crop preserves highlight headroom");
    expect_near(crop.samples[1], -0.5F, "scene-linear crop preserves negative gamut components");
    expect_near(crop.samples[2], 0.125F, "scene-linear crop preserves finite samples");
}

void cross_unit_boundary_rejects_hidden_precondition_violations() {
    const image::DevelopedSourcePixels source = padded_grayscale_source();
    expect_decode_error(
        [&] {
            static_cast<void>(
                proxy_detail::resize_developed_source_to_working(source, image::Dimensions{0U, 1U})
            );
        },
        image::DecodeErrorCode::invalid_request,
        "developed source resize rejects an empty target"
    );
    expect_decode_error(
        [&] {
            static_cast<void>(proxy_detail::crop_developed_source_to_working(
                source,
                image::GeometryPixelRect{.x = 3U, .y = 0U, .width = 1U, .height = 1U}
            ));
        },
        image::DecodeErrorCode::invalid_request,
        "developed source crop rejects a rectangle outside the source"
    );

    image::DevelopedSourcePixels truncated = padded_grayscale_source();
    std::get<image::PixelBuffer>(truncated).samples.pop_back();
    expect_decode_error(
        [&] { proxy_detail::validate_developed_source(truncated); },
        image::DecodeErrorCode::corrupt_data,
        "developed source validation rejects a truncated padded row"
    );
}

void scene_linear_crop_validates_only_the_requested_samples() {
    image::SceneLinearRgbFrame frame{
        .dimensions = {4, 2},
        .row_stride_bytes = 4 * 3 * sizeof(float),
        .samples = std::vector<float>(24, 0.5F)
    };
    frame.samples[21] = std::numeric_limits<float>::quiet_NaN();
    expect(
        frame.valid_layout() && !frame.valid(),
        "layout and complete sample validation must remain distinct"
    );
    expect_decode_error(
        [&] { proxy_detail::validate_developed_source(frame); },
        image::DecodeErrorCode::corrupt_data,
        "session admission must reject a non-finite sample anywhere in the source"
    );
    const auto tile = proxy_detail::crop_developed_source_to_working(
        frame,
        {.x = 0, .y = 0, .width = 1, .height = 1}
    );
    expect(
        tile.samples == std::vector<float>(3, 0.5F),
        "bounded crop reads only the requested region"
    );
    expect_decode_error(
        [&] {
            static_cast<void>(proxy_detail::crop_developed_source_to_working(
                frame,
                {.x = 3, .y = 1, .width = 1, .height = 1}
            ));
        },
        image::DecodeErrorCode::corrupt_data,
        "bounded crop rejects non-finite samples inside the region"
    );
    frame.samples.pop_back();
    expect(!frame.valid_layout(), "truncated source cannot pass constant-time shape validation");
}

} // namespace

int main() {
    scene_linear_crop_validates_only_the_requested_samples();
    pixel_buffer_dispatch_preserves_padding_and_grayscale();
    scene_linear_dispatch_preserves_unbounded_samples();
    cross_unit_boundary_rejects_hidden_precondition_violations();
    return failures == 0 ? 0 : 1;
}
