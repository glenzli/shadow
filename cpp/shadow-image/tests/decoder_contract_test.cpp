#include <shadow/image/decoder.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
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

void pending_corrections_are_explicit() {
    image::PendingCorrections empty;
    expect(!empty.has_pending(), "empty correction state must not be pending");

    image::PendingCorrections stage_three{{0U, 0U, 76U}};
    expect(stage_three.has_pending(), "a DNG opcode list must be reported as pending");
}

void largest_decodable_preview_wins() {
    const std::array previews{
        image::PreviewDescriptor{
            .id = 2,
            .format = image::PreviewFormat::bitmap,
            .dimensions = {160, 120},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 57'600,
            .decodable = true,
        },
        image::PreviewDescriptor{
            .id = 7,
            .format = image::PreviewFormat::jpeg,
            .dimensions = {3'872, 2'592},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 1'285'213,
            .decodable = true,
        },
        image::PreviewDescriptor{
            .id = 9,
            .format = image::PreviewFormat::unknown,
            .dimensions = {8'000, 6'000},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 2'000'000,
            .decodable = false,
        },
    };

    const auto selected = image::select_best_preview(previews);
    expect(selected.has_value(), "a decodable preview should be selected");
    expect(selected == 7U, "selection returns the provider preview id, not the vector index");
}

void no_decodable_preview_is_a_valid_state() {
    const std::array previews{
        image::PreviewDescriptor{
            .id = 0,
            .format = image::PreviewFormat::unknown,
            .dimensions = {},
            .bits_per_channel = 0,
            .channels = 0,
            .encoded_bytes = 0,
            .decodable = false,
        },
    };
    expect(
        !image::select_best_preview(previews).has_value(),
        "files without an embedded preview must remain importable"
    );
}

class FakeRgbSession final : public image::DecodeSession {
public:
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
        buffer.dimensions = {8, 4};
        buffer.bits_per_channel = 16;
        buffer.channels = 3;
        buffer.row_stride_bytes = 8U * 3U * sizeof(std::uint16_t);
        buffer.samples.resize(8U * 4U * 3U);
        for (std::size_t index = 0; index < buffer.samples.size(); ++index) {
            buffer.samples[index] = static_cast<std::uint16_t>((index * 997U) % 65'536U);
        }
        return buffer;
    }

private:
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
};

void reference_proxy_is_bounded_standard_jpeg() {
    expect(
        image::proxy_dimensions({4'032, 3'024}, 2'048) == image::Dimensions{2'048, 1'536},
        "proxy dimensions preserve aspect ratio and max edge"
    );

    const FakeRgbSession session;
    const auto proxy = image::render_reference_proxy_jpeg(
        session,
        image::ProxyRequest{.max_edge = 4, .jpeg_quality = 88}
    );
    expect(proxy.dimensions == image::Dimensions{4, 2}, "proxy renderer downsizes RGB");
    expect(proxy.format == image::PreviewFormat::jpeg, "proxy output is JPEG");
    expect(proxy.bytes.size() > 4U, "proxy JPEG is not empty");
    expect(
        proxy.bytes[0] == 0xffU && proxy.bytes[1] == 0xd8U,
        "proxy output starts with JPEG SOI"
    );
    expect(
        proxy.bytes[proxy.bytes.size() - 2U] == 0xffU && proxy.bytes.back() == 0xd9U,
        "proxy output ends with JPEG EOI"
    );
}

} // namespace

int main() {
    pending_corrections_are_explicit();
    largest_decodable_preview_wins();
    no_decodable_preview_is_a_valid_state();
    reference_proxy_is_bounded_standard_jpeg();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
