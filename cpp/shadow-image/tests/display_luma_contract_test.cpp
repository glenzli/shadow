#include <shadow/image/display_luma.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
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

class GradientRgbSession final : public image::DecodeSession {
public:
    explicit GradientRgbSession(const image::Dimensions dimensions)
        : dimensions_(dimensions) {}

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
        throw image::DecodeError(image::DecodeErrorCode::no_preview, 0, "no preview");
    }

    [[nodiscard]] image::MosaicBuffer decode_mosaic() override {
        throw image::DecodeError(image::DecodeErrorCode::unsupported, 0, "no mosaic");
    }

    [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
        image::PixelBuffer buffer;
        buffer.dimensions = dimensions_;
        buffer.bits_per_channel = 16U;
        buffer.channels = 3U;
        buffer.row_stride_bytes =
            static_cast<std::size_t>(dimensions_.width) * 3U * sizeof(std::uint16_t);
        buffer.samples.resize(
            static_cast<std::size_t>(dimensions_.pixel_count()) * 3U
        );
        for (std::uint32_t row = 0U; row < dimensions_.height; ++row) {
            for (std::uint32_t column = 0U; column < dimensions_.width; ++column) {
                const std::size_t index =
                    (static_cast<std::size_t>(row) * dimensions_.width + column) * 3U;
                buffer.samples[index] = static_cast<std::uint16_t>(
                    (static_cast<std::uint64_t>(column) * 65'535U)
                    / std::max(1U, dimensions_.width - 1U)
                );
                buffer.samples[index + 1U] = static_cast<std::uint16_t>(
                    (static_cast<std::uint64_t>(row) * 65'535U)
                    / std::max(1U, dimensions_.height - 1U)
                );
                buffer.samples[index + 2U] = static_cast<std::uint16_t>(
                    ((static_cast<std::uint64_t>(column) + row) * 65'535U)
                    / std::max(1U, dimensions_.width + dimensions_.height - 2U)
                );
            }
        }
        return buffer;
    }

private:
    image::Dimensions dimensions_;
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
};

[[nodiscard]] std::vector<std::uint8_t> make_jpeg(const image::Dimensions dimensions) {
    const GradientRgbSession session(dimensions);
    return image::render_reference_proxy_jpeg(
               session,
               image::ProxyRequest{
                   .max_edge = std::max(dimensions.width, dimensions.height),
                   .jpeg_quality = 92U,
               }
    )
        .bytes;
}

[[nodiscard]] bool overwrite_sof_dimensions(
    std::vector<std::uint8_t>& jpeg,
    const std::uint16_t width,
    const std::uint16_t height,
    const bool progressive = false,
    const std::uint8_t precision = 8U
) {
    for (std::size_t index = 0U; index + 8U < jpeg.size(); ++index) {
        if (jpeg[index] != 0xffU) {
            continue;
        }
        const std::uint8_t marker = jpeg[index + 1U];
        const bool is_start_of_frame = marker >= 0xc0U && marker <= 0xcfU
            && marker != 0xc4U && marker != 0xc8U && marker != 0xccU;
        if (!is_start_of_frame) {
            continue;
        }
        if (progressive) {
            jpeg[index + 1U] = 0xc2U;
        }
        jpeg[index + 4U] = precision;
        jpeg[index + 5U] = static_cast<std::uint8_t>(height >> 8U);
        jpeg[index + 6U] = static_cast<std::uint8_t>(height & 0xffU);
        jpeg[index + 7U] = static_cast<std::uint8_t>(width >> 8U);
        jpeg[index + 8U] = static_cast<std::uint8_t>(width & 0xffU);
        return true;
    }
    return false;
}

void legal_jpeg_produces_normalized_tightly_packed_luma() {
    const auto jpeg = make_jpeg({16U, 8U});
    const auto luma = image::decode_jpeg_display_luma(jpeg, 512U);

    expect(luma.dimensions == image::Dimensions{16U, 8U}, "small JPEG keeps its dimensions");
    expect(luma.row_stride_samples == 16U, "display-luma stride is tightly packed");
    expect(luma.samples.size() == 16U * 8U, "display-luma sample count matches dimensions");
    expect(
        luma.preprocessing_version
            == std::string(image::jpeg_display_luma_preprocessing_version_prefix())
                + ":max-edge-512",
        "display-luma reports its exact JPEG preprocessing revision"
    );
    expect(
        std::ranges::all_of(luma.samples, [](const float sample) {
            return std::isfinite(sample) && sample >= 0.0F && sample <= 1.0F;
        }),
        "all display-luma samples are finite and normalized"
    );
    expect(
        *std::ranges::min_element(luma.samples) < *std::ranges::max_element(luma.samples),
        "a gradient JPEG produces non-constant display luma"
    );
}

void idct_and_final_resize_are_bounded_and_deterministic() {
    const auto jpeg = make_jpeg({1'024U, 256U});
    const auto first = image::decode_jpeg_display_luma(jpeg, 512U);
    const auto second = image::decode_jpeg_display_luma(jpeg, 512U);
    expect(
        first.dimensions == image::Dimensions{512U, 128U},
        "large JPEG is resized to the requested analysis edge"
    );
    expect(first.samples == second.samples, "repeated display-luma decoding is deterministic");
    expect(
        first.preprocessing_version == second.preprocessing_version,
        "deterministic runs retain identical preprocessing provenance"
    );
    expect(
        first.preprocessing_version.find("libjpeg-turbo-") != std::string::npos
            && first.preprocessing_version.find(":islow:no-fancy-upsampling:")
                != std::string::npos,
        "provenance pins the JPEG implementation and deterministic decoder settings"
    );

    const auto smaller = image::decode_jpeg_display_luma(jpeg, 64U);
    expect(
        smaller.dimensions == image::Dimensions{64U, 16U},
        "a smaller valid analysis bound is honored"
    );
    expect(
        smaller.preprocessing_version.ends_with(":max-edge-64"),
        "the requested analysis scale is part of preprocessing provenance"
    );
}

void corrupt_and_truncated_jpegs_fail_closed() {
    const std::vector<std::uint8_t> corrupt{0xffU, 0xd8U, 0xffU, 0x00U, 0x17U};
    try {
        static_cast<void>(image::decode_jpeg_display_luma(corrupt, 512U));
        expect(false, "corrupt JPEG must fail");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::corrupt_data,
            "corrupt JPEG reports corrupt data"
        );
    }

    auto truncated = make_jpeg({32U, 16U});
    truncated.resize(truncated.size() - 2U);
    try {
        static_cast<void>(image::decode_jpeg_display_luma(truncated, 512U));
        expect(false, "truncated JPEG must fail instead of accepting a synthesized EOI");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::corrupt_data,
            "truncated JPEG warning is promoted to corrupt data"
        );
    }

    auto unsupported_precision = make_jpeg({8U, 8U});
    expect(
        overwrite_sof_dimensions(unsupported_precision, 8U, 8U, false, 12U),
        "test JPEG contains a mutable frame precision"
    );
    try {
        static_cast<void>(image::decode_jpeg_display_luma(unsupported_precision, 512U));
        expect(false, "non-8-bit JPEG must fail the preprocessing contract");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported_layout,
            "unsupported JPEG precision is distinct from corrupt data"
        );
    }
}

void resource_and_request_limits_fail_before_pixel_decode() {
    auto oversized_header = make_jpeg({8U, 8U});
    expect(
        overwrite_sof_dimensions(oversized_header, 20'000U, 20'000U),
        "test JPEG contains a start-of-frame marker"
    );
    try {
        static_cast<void>(image::decode_jpeg_display_luma(oversized_header, 512U));
        expect(false, "oversized JPEG header must fail before decompression");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::resource_limit,
            "oversized source pixels report a resource limit"
        );
    }

    auto oversized_progressive_header = make_jpeg({8U, 8U});
    expect(
        overwrite_sof_dimensions(
            oversized_progressive_header,
            8'000U,
            7'000U,
            true
        ),
        "test JPEG can advertise a multi-scan frame"
    );
    try {
        static_cast<void>(
            image::decode_jpeg_display_luma(oversized_progressive_header, 512U)
        );
        expect(false, "large multi-scan JPEG must fail before allocating full-image state");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::resource_limit,
            "large multi-scan source reports a resource limit"
        );
    }

    for (const std::uint32_t invalid_edge : {0U, 513U}) {
        try {
            static_cast<void>(image::decode_jpeg_display_luma(oversized_header, invalid_edge));
            expect(false, "invalid output edge must fail");
        } catch (const image::DecodeError& error) {
            expect(
                error.code() == image::DecodeErrorCode::invalid_request,
                "invalid output edge is rejected before reading JPEG data"
            );
        }
    }
}

} // namespace

int main() {
    legal_jpeg_produces_normalized_tightly_packed_luma();
    idct_and_final_resize_are_bounded_and_deterministic();
    corrupt_and_truncated_jpegs_fail_closed();
    resource_and_request_limits_fail_before_pixel_decode();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
