#pragma once

#include "contract_test_assertions.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/full_edit_detail.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace shadow::image::test_support {

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

} // namespace shadow::image::test_support
