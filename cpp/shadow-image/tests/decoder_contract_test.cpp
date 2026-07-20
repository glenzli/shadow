#include <shadow/image/decoder.hpp>
#include <shadow/image/edit.hpp>

#include <array>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
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

class BoundaryRgbSession final : public image::DecodeSession {
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
        return image::PixelBuffer{
            .dimensions = {5, 1},
            .bits_per_channel = 16,
            .channels = 3,
            .row_stride_bytes = 5U * 3U * sizeof(std::uint16_t),
            .color_space = image::ColorSpace::srgb,
            .samples = {
                0U, 0U, 0U,
                65'535U, 65'535U, 65'535U,
                65'535U, 0U, 0U,
                0U, 65'535U, 0U,
                0U, 0U, 65'535U,
            },
        };
    }

    [[nodiscard]] std::size_t reference_render_count() const noexcept {
        return reference_render_count_;
    }

private:
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
    mutable std::size_t reference_render_count_ = 0;
};

template <std::size_t Size>
[[nodiscard]] std::uint64_t sum_counts(const std::array<std::uint64_t, Size>& values) {
    std::uint64_t sum = 0U;
    for (const auto value : values) {
        sum += value;
    }
    return sum;
}

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
            .node_id = "tone-curve",
            .parameters = image::ToneCurve{},
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
    adjusted_nodes[3].parameters = image::ChannelGainAdjustment{{1.1, 1.0, 0.9}};
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

void warm_edit_preview_analysis_is_pre_jpeg_and_strictly_pre_clamp() {
    const BoundaryRgbSession session;
    const auto warm = image::prepare_warm_edit_preview(session, 5);
    expect(session.reference_render_count() == 1U, "analysis preparation decodes exactly once");

    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "neutral-exposure",
            .parameters = image::ExposureAdjustment{},
        },
    };
    const auto low_quality = warm.render_jpeg_with_analysis(neutral_nodes, 1);
    const auto high_quality = warm.render_jpeg_with_analysis(neutral_nodes, 100);
    const auto& neutral = low_quality.analysis;

    expect(
        neutral == high_quality.analysis,
        "JPEG quality cannot affect analysis computed from pre-encode RGB8"
    );
    expect(
        low_quality.proxy.bytes != high_quality.proxy.bytes,
        "the quality-independence check still exercises distinct JPEG encodings"
    );
    expect(
        neutral.sample_dimensions == image::Dimensions{5, 1} && neutral.pixel_count == 5U,
        "analysis describes the complete warm proxy"
    );
    expect(
        neutral.red[0] == 3U && neutral.red[255] == 2U
            && neutral.green[0] == 3U && neutral.green[255] == 2U
            && neutral.blue[0] == 3U && neutral.blue[255] == 2U,
        "known black, white, and primary pixels land in exact RGB endpoint bins"
    );
    expect(
        neutral.luma[0] == 1U && neutral.luma[255] == 1U
            && neutral.luma[54] == 1U && neutral.luma[182] == 1U
            && neutral.luma[18] == 1U,
        "fixed-point Rec.709 encoded luma preserves endpoints and RGB channel order"
    );
    expect(
        sum_counts(neutral.red) == neutral.pixel_count
            && sum_counts(neutral.green) == neutral.pixel_count
            && sum_counts(neutral.blue) == neutral.pixel_count
            && sum_counts(neutral.luma) == neutral.pixel_count,
        "every histogram contains exactly one sample per proxy pixel"
    );
    expect(
        neutral.below_zero_samples == std::array<std::uint64_t, 3>{0U, 0U, 0U}
            && neutral.above_one_samples == std::array<std::uint64_t, 3>{0U, 0U, 0U}
            && neutral.shadow_clipped_pixels == 0U
            && neutral.highlight_clipped_pixels == 0U,
        "exact scene-linear zero and one are legal and are not reported as clipped"
    );

    const std::array highlight_nodes{
        image::AdjustmentNode{
            .node_id = "highlight-exposure",
            .parameters = image::ExposureAdjustment{1.0},
        },
    };
    const auto highlight = warm.render_jpeg_with_analysis(highlight_nodes, 80).analysis;
    expect(
        highlight.above_one_samples == std::array<std::uint64_t, 3>{2U, 2U, 2U}
            && highlight.highlight_clipped_pixels == 4U,
        "super-white channels and their any-channel pixel union are counted independently"
    );

    image::ToneCurve lowered_curve;
    lowered_curve.points = {{0.0, -0.1}, {1.0, 0.9}};
    const std::array shadow_nodes{
        image::AdjustmentNode{
            .node_id = "lowered-curve",
            .parameters = std::move(lowered_curve),
        },
    };
    const auto shadow = warm.render_jpeg_with_analysis(shadow_nodes, 80).analysis;
    expect(
        shadow.below_zero_samples == std::array<std::uint64_t, 3>{3U, 3U, 3U}
            && shadow.shadow_clipped_pixels == 4U,
        "negative channels and their any-channel pixel union are counted independently"
    );
    expect(
        session.reference_render_count() == 1U,
        "repeated analyzed renders never ask the decoder for pixels again"
    );

    auto first_concurrent = std::async(std::launch::async, [&warm, &neutral_nodes]() {
        return warm.render_jpeg_with_analysis(neutral_nodes, 80);
    });
    auto second_concurrent = std::async(std::launch::async, [&warm, &neutral_nodes]() {
        return warm.render_jpeg_with_analysis(neutral_nodes, 80);
    });
    const auto first_result = first_concurrent.get();
    const auto second_result = second_concurrent.get();
    expect(
        first_result.analysis == second_result.analysis
            && first_result.proxy.bytes == second_result.proxy.bytes,
        "concurrent const analyzed renders are deterministic and isolated"
    );
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

void edited_proxy_rejects_invalid_nodes_before_decode() {
    const FakeRgbSession session;
    const image::ProxyRequest request{.max_edge = 4, .jpeg_quality = 90};

    const auto rejects_before_decode = [&](const image::AdjustmentNode& invalid_node,
                                           const image::EditErrorCode expected_code,
                                           const std::string_view message) {
        const std::array nodes{invalid_node};
        try {
            static_cast<void>(image::render_edited_reference_proxy_jpeg(
                session,
                nodes,
                request
            ));
            expect(false, message);
        } catch (const image::EditError& error) {
            expect(error.code() == expected_code, message);
            expect(error.node_index() == 0U, "preflight errors retain node provenance");
        }
        expect(
            session.reference_render_count() == 0U,
            "adjustment preflight rejects invalid nodes before rendering reference RGB"
        );
    };

    rejects_before_decode(
        image::AdjustmentNode{
            .node_id = "non-finite-disabled-exposure",
            .enabled = false,
            .parameters = image::ExposureAdjustment{
                std::numeric_limits<double>::quiet_NaN(),
            },
        },
        image::EditErrorCode::invalid_parameter,
        "preflight validates numeric parameters even on disabled nodes"
    );

    rejects_before_decode(
        image::AdjustmentNode{
            .node_id = "future-version",
            .implementation_version = image::adjustment_implementation_version + 1U,
            .parameters = image::ExposureAdjustment{},
        },
        image::EditErrorCode::unsupported_version,
        "preflight rejects unsupported adjustment implementations"
    );

    image::ToneCurve overflowing_slope;
    overflowing_slope.points = {
        {0.0, 0.0},
        {
            std::numeric_limits<double>::min(),
            std::numeric_limits<double>::max(),
        },
        {1.0, 1.0},
    };
    rejects_before_decode(
        image::AdjustmentNode{
            .node_id = "overflowing-tone-curve-slope",
            .parameters = std::move(overflowing_slope),
        },
        image::EditErrorCode::invalid_parameter,
        "preflight rejects non-finite Tone Curve segment slopes"
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
    warm_edit_preview_analysis_is_pre_jpeg_and_strictly_pre_clamp();
    warm_edit_preview_bounds_fail_before_decode();
    edited_proxy_rejects_invalid_nodes_before_decode();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
