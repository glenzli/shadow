#include <shadow/image/decoder.hpp>
#include <shadow/image/edit.hpp>

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
        ++reference_render_count_;
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

    [[nodiscard]] std::size_t reference_render_count() const noexcept {
        return reference_render_count_;
    }

private:
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
    mutable std::size_t reference_render_count_ = 0;
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

void edited_proxy_crosses_explicit_linear_srgb_boundary() {
    const FakeRgbSession session;
    const image::ProxyRequest request{.max_edge = 8, .jpeg_quality = 90};
    const auto reference = image::render_reference_proxy_jpeg(session, request);
    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "contrast",
            .parameters = image::ContrastAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "channel-gain",
            .parameters = image::ChannelGainAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{},
        },
    };
    const auto neutral = image::render_edited_reference_proxy_jpeg(
        session,
        neutral_nodes,
        request
    );
    expect(
        neutral.bytes == reference.bytes,
        "neutral edits round-trip the sRGB transfer boundary without changing unscaled pixels"
    );

    auto adjusted_nodes = neutral_nodes;
    adjusted_nodes[0].parameters = image::ExposureAdjustment{1.0};
    adjusted_nodes[2].parameters = image::ChannelGainAdjustment{{1.1, 1.0, 0.9}};
    const image::ProxyRequest small_request{.max_edge = 4, .jpeg_quality = 90};
    const auto neutral_small = image::render_edited_reference_proxy_jpeg(
        session,
        neutral_nodes,
        small_request
    );
    const auto adjusted = image::render_edited_reference_proxy_jpeg(
        session,
        adjusted_nodes,
        small_request
    );
    expect(adjusted.dimensions == image::Dimensions{4, 2}, "edited preview remains bounded");
    expect(adjusted.bytes != neutral_small.bytes, "ordered edit nodes affect the encoded result");
    expect(
        adjusted.bytes.size() > 4U && adjusted.bytes[0] == 0xffU
            && adjusted.bytes[1] == 0xd8U
            && adjusted.bytes[adjusted.bytes.size() - 2U] == 0xffU
            && adjusted.bytes.back() == 0xd9U,
        "edited preview is a standard JPEG"
    );
}

void warm_edit_preview_decodes_once_and_renders_repeatedly() {
    const FakeRgbSession session;
    const auto warm = image::prepare_warm_edit_preview(session, 4);
    expect(session.reference_render_count() == 1U, "warm preparation renders the RAW once");
    expect(warm.dimensions() == image::Dimensions{4, 2}, "warm working proxy is max-edge bounded");
    expect(warm.max_edge() == 4U, "warm working proxy remembers its resource bound");

    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{},
        },
    };
    auto adjusted_nodes = neutral_nodes;
    adjusted_nodes[0].parameters = image::ExposureAdjustment{1.0};

    const auto neutral = warm.render_jpeg(neutral_nodes, 90);
    const auto adjusted = warm.render_jpeg(adjusted_nodes, 90);
    expect(
        session.reference_render_count() == 1U,
        "repeated warm renders never ask the decoder for pixels again"
    );
    expect(neutral.dimensions == image::Dimensions{4, 2}, "warm output dimensions stay fixed");
    expect(adjusted.dimensions == neutral.dimensions, "all warm renders share working dimensions");
    expect(adjusted.bytes != neutral.bytes, "warm renders apply each requested edit independently");

    const auto one_shot_adjusted = image::render_edited_reference_proxy_jpeg(
        session,
        adjusted_nodes,
        image::ProxyRequest{.max_edge = 4, .jpeg_quality = 90}
    );
    expect(
        adjusted.bytes == one_shot_adjusted.bytes,
        "linear affine edits commute with the warm proxy's linear downsampling"
    );
    expect(session.reference_render_count() == 2U, "only the one-shot comparison decodes again");
}

void warm_edit_preview_bounds_fail_before_decode() {
    const FakeRgbSession session;
    try {
        static_cast<void>(image::prepare_warm_edit_preview(session, 0));
        expect(false, "zero warm edge must fail");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "zero warm edge reports an invalid request"
        );
    }
    try {
        static_cast<void>(image::prepare_warm_edit_preview(
            session,
            image::maximum_warm_edit_preview_edge + 1U
        ));
        expect(false, "oversized warm edge must fail");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "oversized warm edge reports an invalid request"
        );
    }
    expect(
        session.reference_render_count() == 0U,
        "invalid warm bounds are rejected before decoder work"
    );
}

} // namespace

int main() {
    pending_corrections_are_explicit();
    largest_decodable_preview_wins();
    no_decodable_preview_is_a_valid_state();
    reference_proxy_is_bounded_standard_jpeg();
    edited_proxy_crosses_explicit_linear_srgb_boundary();
    warm_edit_preview_decodes_once_and_renders_repeatedly();
    warm_edit_preview_bounds_fail_before_decode();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
