#include <shadow/image/raw_foundation.hpp>

#include "../concurrency/row_scheduler.hpp"
#include "bayer_sampling.hpp"
#include "raw_frame_region_development.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/proxy_rendering.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

[[nodiscard]] bool canonical_sha256(const std::string_view value) noexcept {
    return value.size() == 64U && std::all_of(value.begin(), value.end(), [](const char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

[[nodiscard]] Dimensions
oriented_dimensions(const Dimensions dimensions, const std::int32_t orientation) noexcept {
    return raw_pipeline_detail::oriented_raw_dimensions(dimensions, orientation);
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> source_coordinate(
    const std::uint32_t output_x,
    const std::uint32_t output_y,
    const Dimensions source,
    const std::int32_t orientation
) noexcept {
    switch (orientation) {
    case 3:
        return {source.width - 1U - output_x, source.height - 1U - output_y};
    case 5:
        return {source.width - 1U - output_y, output_x};
    case 6:
        return {output_y, source.height - 1U - output_x};
    case 0:
    default:
        return {output_x, output_y};
    }
}

[[nodiscard]] std::size_t checked_sample_count(const Dimensions dimensions) {
    const std::uint64_t pixel_count = dimensions.pixel_count();
    const std::uint64_t maximum_samples =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) / 3U;
    if (pixel_count == 0U || pixel_count > maximum_samples) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "AI RAW foundation raster exceeds the native address space"
        );
    }
    return static_cast<std::size_t>(pixel_count * 3U);
}

[[nodiscard]] std::array<float, 3U> sample_bilinear(
    const RawFoundationCameraRgbView& source,
    const Dimensions target,
    const std::uint32_t x,
    const std::uint32_t y
) noexcept {
    const double source_x = (static_cast<double>(x) + 0.5)
                                * static_cast<double>(source.dimensions.width)
                                / static_cast<double>(target.width)
                            - 0.5;
    const double source_y = (static_cast<double>(y) + 0.5)
                                * static_cast<double>(source.dimensions.height)
                                / static_cast<double>(target.height)
                            - 0.5;
    const double clamped_x =
        std::clamp(source_x, 0.0, static_cast<double>(source.dimensions.width - 1U));
    const double clamped_y =
        std::clamp(source_y, 0.0, static_cast<double>(source.dimensions.height - 1U));
    const auto x0 = static_cast<std::uint32_t>(std::floor(clamped_x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clamped_y));
    const auto x1 = std::min(x0 + 1U, source.dimensions.width - 1U);
    const auto y1 = std::min(y0 + 1U, source.dimensions.height - 1U);
    const float tx = static_cast<float>(clamped_x - static_cast<double>(x0));
    const float ty = static_cast<float>(clamped_y - static_cast<double>(y0));
    const auto at = [&](const std::uint32_t sample_x,
                        const std::uint32_t sample_y,
                        const std::size_t channel) noexcept {
        const std::size_t index =
            (static_cast<std::size_t>(sample_y) * source.dimensions.width + sample_x) * 3U
            + channel;
        return source.samples[index];
    };
    std::array<float, 3U> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel) {
        const float top = std::lerp(at(x0, y0, channel), at(x1, y0, channel), tx);
        const float bottom = std::lerp(at(x0, y1, channel), at(x1, y1, channel), tx);
        result[channel] = std::lerp(top, bottom, ty);
    }
    return result;
}

[[nodiscard]] std::array<float, 3U> sample_original_bilinear(
    const RawFrame& frame,
    const RawFoundationCameraRgbView& foundation,
    const Dimensions target,
    const std::uint32_t x,
    const std::uint32_t y
) noexcept {
    const double foundation_x = (static_cast<double>(x) + 0.5)
                                    * static_cast<double>(foundation.dimensions.width)
                                    / static_cast<double>(target.width)
                                - 0.5;
    const double foundation_y = (static_cast<double>(y) + 0.5)
                                    * static_cast<double>(foundation.dimensions.height)
                                    / static_cast<double>(target.height)
                                - 0.5;
    const auto& descriptor = frame.descriptor;
    const double active_x = std::clamp(
        foundation_x + static_cast<double>(foundation.crop_left),
        0.0,
        static_cast<double>(descriptor.active_dimensions.width - 1U)
    );
    const double active_y = std::clamp(
        foundation_y + static_cast<double>(foundation.crop_top),
        0.0,
        static_cast<double>(descriptor.active_dimensions.height - 1U)
    );
    const auto x0 = static_cast<std::uint32_t>(std::floor(active_x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(active_y));
    const auto x1 = std::min(x0 + 1U, descriptor.active_dimensions.width - 1U);
    const auto y1 = std::min(y0 + 1U, descriptor.active_dimensions.height - 1U);
    const float tx = static_cast<float>(active_x - static_cast<double>(x0));
    const float ty = static_cast<float>(active_y - static_cast<double>(y0));
    const std::uint32_t raw_left = descriptor.active_margins.left;
    const std::uint32_t raw_top = descriptor.active_margins.top;
    const auto top_left = detail::bilinear_camera_rgb_at(frame, raw_left + x0, raw_top + y0);
    const auto top_right = detail::bilinear_camera_rgb_at(frame, raw_left + x1, raw_top + y0);
    const auto bottom_left = detail::bilinear_camera_rgb_at(frame, raw_left + x0, raw_top + y1);
    const auto bottom_right = detail::bilinear_camera_rgb_at(frame, raw_left + x1, raw_top + y1);
    std::array<float, 3U> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel) {
        const float top = std::lerp(top_left[channel], top_right[channel], tx);
        const float bottom = std::lerp(bottom_left[channel], bottom_right[channel], tx);
        result[channel] = std::lerp(top, bottom, ty);
    }
    return result;
}

[[nodiscard]] DevelopedRawFoundation develop_raw_foundation_impl(
    const RawFoundationCameraRgbView& foundation,
    const RawFrameDescriptor& source_descriptor,
    const RawFrame* const source_frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge
) {
    if (!foundation.matches_source(source_descriptor)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "AI RAW foundation geometry does not match the source active sensor"
        );
    }
    if (foundation.amount_percent < 100U && source_frame == nullptr) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "AI RAW foundation amount blending requires the decoded original RAW frame"
        );
    }
    if (source_frame != nullptr) {
        detail::validate_bayer_frame(*source_frame, "AI RAW foundation amount blending");
    }
    if (!transform.valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "AI RAW foundation requires a finite camera-to-working transform"
        );
    }
    if (preview_max_edge.has_value() && *preview_max_edge == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "AI RAW foundation preview edge must be non-zero"
        );
    }

    const Dimensions camera_rgb_dimensions =
        preview_max_edge.has_value() ? proxy_dimensions(foundation.dimensions, *preview_max_edge)
                                     : foundation.dimensions;
    const Dimensions output_dimensions =
        oriented_dimensions(camera_rgb_dimensions, source_descriptor.orientation);
    SceneLinearRgbFrame output{
        .dimensions = output_dimensions,
        .row_stride_bytes = static_cast<std::size_t>(output_dimensions.width) * 3U * sizeof(float),
        .samples = std::vector<float>(checked_sample_count(output_dimensions)),
    };
    const float ai_amount = static_cast<float>(foundation.amount_percent) / 100.0F;
    detail::parallel_for_rows(
        output_dimensions.height,
        8U,
        [&](const std::uint32_t first_row, const std::uint32_t last_row) {
            for (std::uint32_t output_y = first_row; output_y < last_row; ++output_y) {
                for (std::uint32_t output_x = 0U; output_x < output_dimensions.width; ++output_x) {
                    const auto [camera_x, camera_y] = source_coordinate(
                        output_x,
                        output_y,
                        camera_rgb_dimensions,
                        source_descriptor.orientation
                    );
                    auto camera =
                        sample_bilinear(foundation, camera_rgb_dimensions, camera_x, camera_y);
                    if (source_frame != nullptr && foundation.amount_percent < 100U) {
                        const auto original = sample_original_bilinear(
                            *source_frame,
                            foundation,
                            camera_rgb_dimensions,
                            camera_x,
                            camera_y
                        );
                        for (std::size_t channel = 0U; channel < camera.size(); ++channel) {
                            camera[channel] =
                                std::lerp(original[channel], camera[channel], ai_amount);
                        }
                    }
                    const std::size_t output_index =
                        (static_cast<std::size_t>(output_y) * output_dimensions.width + output_x)
                        * 3U;
                    for (std::size_t output_channel = 0U; output_channel < 3U; ++output_channel) {
                        double value = 0.0;
                        for (std::size_t input_channel = 0U; input_channel < 3U; ++input_channel) {
                            value +=
                                transform
                                    .camera_to_linear_srgb_d65[output_channel * 3U + input_channel]
                                * static_cast<double>(camera[input_channel]);
                        }
                        output.samples[output_index + output_channel] = static_cast<float>(value);
                    }
                }
            }
        }
    );
    DevelopedRawFoundation developed{
        .scene_linear = std::move(output),
        .camera_rgb_dimensions = camera_rgb_dimensions,
        .bounded_preview = camera_rgb_dimensions != foundation.dimensions,
        .cache_identity =
            foundation.provenance.cache_identity()
            + ";amount-percent=" + std::to_string(foundation.amount_percent),
    };
    if (!developed.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "AI RAW foundation produced an invalid scene-linear raster"
        );
    }
    return developed;
}

} // namespace

bool RawFoundationProvenance::valid() const noexcept {
    return canonical_sha256(source_sha256) && canonical_sha256(artifact_file_sha256)
           && canonical_sha256(cache_key_sha256) && model_identity == raw_foundation_model_identity
           && implementation_revision == raw_foundation_implementation_revision;
}

std::string RawFoundationProvenance::cache_identity() const {
    if (!valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "AI RAW foundation provenance is invalid"
        );
    }
    return "raw-foundation=" + model_identity + ";implementation=" + implementation_revision
           + ";source-sha256=" + source_sha256 + ";artifact-sha256=" + artifact_file_sha256
           + ";cache-key-sha256=" + cache_key_sha256;
}

bool RawFoundationCameraRgbView::valid() const noexcept {
    if (dimensions.width == 0U || dimensions.height == 0U || crop_top > 1U || crop_left > 1U
        || amount_percent > 100U || !provenance.valid()) {
        return false;
    }
    const std::uint64_t pixel_count = dimensions.pixel_count();
    const std::uint64_t maximum_samples =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) / 3U;
    if (pixel_count > maximum_samples
        || samples.size() != static_cast<std::size_t>(pixel_count * 3U)) {
        return false;
    }
    return std::all_of(samples.begin(), samples.end(), [](const float value) {
        return std::isfinite(value);
    });
}

bool RawFoundationCameraRgbView::matches_source(
    const RawFrameDescriptor& descriptor
) const noexcept {
    if (!valid() || descriptor.schema_version != raw_frame_schema_version
        || descriptor.cfa_layout != RawFrameCfaLayout::bayer_2x2
        || descriptor.active_dimensions.width == 0U || descriptor.active_dimensions.height == 0U
        || crop_left > descriptor.active_dimensions.width
        || crop_top > descriptor.active_dimensions.height) {
        return false;
    }
    const std::uint64_t used_width = static_cast<std::uint64_t>(crop_left) + dimensions.width;
    const std::uint64_t used_height = static_cast<std::uint64_t>(crop_top) + dimensions.height;
    if (used_width > descriptor.active_dimensions.width
        || used_height > descriptor.active_dimensions.height) {
        return false;
    }
    return descriptor.active_dimensions.width - used_width <= 1U
           && descriptor.active_dimensions.height - used_height <= 1U;
}

bool DevelopedRawFoundation::valid() const noexcept {
    return scene_linear.valid() && camera_rgb_dimensions.width > 0U
           && camera_rgb_dimensions.height > 0U && !cache_identity.empty();
}

DevelopedRawFoundation develop_raw_foundation(
    const RawFoundationCameraRgbView& foundation,
    const RawFrameDescriptor& source_descriptor,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge
) {
    return develop_raw_foundation_impl(
        foundation,
        source_descriptor,
        nullptr,
        transform,
        preview_max_edge
    );
}

DevelopedRawFoundation develop_raw_foundation(
    const RawFoundationCameraRgbView& foundation,
    const RawFrame& source_frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge
) {
    return develop_raw_foundation_impl(
        foundation,
        source_frame.descriptor,
        &source_frame,
        transform,
        preview_max_edge
    );
}

} // namespace shadow::image
