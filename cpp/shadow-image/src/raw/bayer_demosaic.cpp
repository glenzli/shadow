#include <shadow/image/raw_development.hpp>

#include <array>
#include <cmath>
#include <limits>

namespace shadow::image {

namespace {

[[nodiscard]] int rgb_channel(const RawCfaColor color) noexcept {
    switch (color) {
    case RawCfaColor::red:
        return 0;
    case RawCfaColor::green:
        return 1;
    case RawCfaColor::blue:
        return 2;
    case RawCfaColor::unknown:
        return -1;
    }
    return -1;
}

[[nodiscard]] std::size_t cfa_site(const std::uint32_t x, const std::uint32_t y) noexcept {
    return static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
}

[[nodiscard]] RawCfaColor cfa_color_at(
    const RawFrameDescriptor& descriptor,
    const std::uint32_t x,
    const std::uint32_t y
) noexcept {
    return descriptor.bayer_2x2[cfa_site(x, y)];
}

[[nodiscard]] float normalized_sample(
    const RawFrame& frame,
    const std::uint32_t x,
    const std::uint32_t y
) noexcept {
    const auto& descriptor = frame.descriptor;
    const auto site = cfa_site(x, y);
    const auto width = static_cast<std::size_t>(descriptor.storage_dimensions.width);
    const auto index = static_cast<std::size_t>(y) * width + x;
    const double black = descriptor.black_levels[site];
    const double white = descriptor.white_levels[site];
    return static_cast<float>((static_cast<double>(frame.samples[index]) - black) / (white - black));
}

} // namespace

bool LinearCameraRgbFrame::valid() const noexcept {
    const auto width = static_cast<std::uint64_t>(dimensions.width);
    const auto height = static_cast<std::uint64_t>(dimensions.height);
    if (width == 0U || height == 0U || row_stride_bytes != width * 3U * sizeof(float)) {
        return false;
    }
    const auto sample_count = width * height * 3U;
    if (
        sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        || samples.size() != static_cast<std::size_t>(sample_count)
        || receipt.schema_version != raw_demosaic_receipt_schema_version
        || receipt.source_raw_frame_schema_version != raw_frame_schema_version
        || !receipt.black_subtraction_applied || !receipt.white_level_normalization_applied
        || receipt.white_balance_applied || receipt.dng_opcodes_applied
    ) {
        return false;
    }
    for (const auto sample : samples) {
        if (!std::isfinite(sample)) {
            return false;
        }
    }
    return true;
}

LinearCameraRgbFrame demosaic_bayer_bilinear(const RawFrame& frame) {
    if (!frame.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Bayer demosaic requires a valid owned RAW frame"
        );
    }
    if (!frame.is_bayer_2x2()) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "Bayer demosaic requires an explicit Bayer two-by-two CFA layout"
        );
    }

    const auto& descriptor = frame.descriptor;
    const auto width = descriptor.storage_dimensions.width;
    const auto height = descriptor.storage_dimensions.height;
    const auto active_width = descriptor.active_dimensions.width;
    const auto active_height = descriptor.active_dimensions.height;
    const auto left = descriptor.active_margins.left;
    const auto top = descriptor.active_margins.top;
    const auto output_samples = static_cast<std::uint64_t>(active_width) * active_height * 3U;
    if (output_samples > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "Bayer demosaic active frame exceeds the address space"
        );
    }

    LinearCameraRgbFrame output;
    output.dimensions = descriptor.active_dimensions;
    output.row_stride_bytes = static_cast<std::size_t>(active_width) * 3U * sizeof(float);
    output.samples.resize(static_cast<std::size_t>(output_samples));
    output.receipt = RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = descriptor.schema_version,
        .algorithm = RawDemosaicAlgorithm::bayer_bilinear_v1,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = false,
        .dng_opcodes_applied = false,
    };

    for (std::uint32_t output_y = 0U; output_y < active_height; ++output_y) {
        const auto raw_y = top + output_y;
        for (std::uint32_t output_x = 0U; output_x < active_width; ++output_x) {
            const auto raw_x = left + output_x;
            const auto output_index = (static_cast<std::size_t>(output_y) * active_width + output_x)
                * 3U;
            for (int channel = 0; channel < 3; ++channel) {
                double total = 0.0;
                std::size_t count = 0U;
                for (int dy = -1; dy <= 1; ++dy) {
                    const auto candidate_y = static_cast<std::int64_t>(raw_y) + dy;
                    if (candidate_y < 0 || candidate_y >= static_cast<std::int64_t>(height)) {
                        continue;
                    }
                    for (int dx = -1; dx <= 1; ++dx) {
                        const auto candidate_x = static_cast<std::int64_t>(raw_x) + dx;
                        if (candidate_x < 0 || candidate_x >= static_cast<std::int64_t>(width)) {
                            continue;
                        }
                        const auto x = static_cast<std::uint32_t>(candidate_x);
                        const auto y = static_cast<std::uint32_t>(candidate_y);
                        if (rgb_channel(cfa_color_at(descriptor, x, y)) != channel) {
                            continue;
                        }
                        total += normalized_sample(frame, x, y);
                        ++count;
                    }
                }
                // A valid Bayer 2x2 CFA always has every component within this clipped 3x3
                // neighbourhood, including active-area borders. Keep this guard fail-closed in
                // case a future frame schema expands the pattern rules.
                if (count == 0U) {
                    throw DecodeError(
                        DecodeErrorCode::unsupported_layout,
                        0,
                        "Bayer demosaic found no same-colour neighbour"
                    );
                }
                output.samples[output_index + static_cast<std::size_t>(channel)] =
                    static_cast<float>(total / static_cast<double>(count));
            }
        }
    }

    if (!output.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "Bayer demosaic produced an invalid camera-linear frame"
        );
    }
    return output;
}

} // namespace shadow::image
