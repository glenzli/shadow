#include "lensfun_modifier_plan_internal.hpp"

#include "manual_optics.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace shadow::image::detail::lensfun_modifier_plan {

namespace {

#if SHADOW_IMAGE_HAS_LENSFUN

void validate_reference_input(const PixelBuffer& input, const Dimensions dimensions) {
    if (input.dimensions != dimensions || input.bits_per_channel != 16U
        || input.channels != internal::rgb_channels
        || input.transfer_function != RgbTransferFunction::linear
        || input.primaries != RgbPrimaries::srgb_rec709_d65
        || input.reference != RgbBufferReference::processed_raw) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "Lensfun modifier plan expects linear processed 16-bit sRGB-primary RGB"
        );
    }
    const auto expected = internal::checked_sample_count(
        dimensions.width,
        dimensions.height,
        internal::rgb_channels,
        "Lensfun packed input dimensions overflow the address space"
    );
    const auto expected_stride =
        static_cast<std::size_t>(dimensions.width) * internal::rgb_channels * sizeof(std::uint16_t);
    if (input.row_stride_bytes != expected_stride || input.samples.size() != expected) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "Lensfun packed input layout does not match its descriptor"
        );
    }
}

template <typename Sample>
[[nodiscard]] Sample bilinear_sample_channel(
    const std::span<const Sample> source,
    const Dimensions dimensions,
    const float source_x,
    const float source_y,
    const std::size_t channel
) noexcept {
    const auto width = static_cast<std::size_t>(dimensions.width);
    const auto height = static_cast<std::size_t>(dimensions.height);
    if (!std::isfinite(source_x) || !std::isfinite(source_y) || source_x < 0.0F || source_y < 0.0F
        || source_x > static_cast<float>(dimensions.width - 1U)
        || source_y > static_cast<float>(dimensions.height - 1U)) {
        return Sample{};
    }
    const auto x0 = static_cast<std::size_t>(std::floor(source_x));
    const auto y0 = static_cast<std::size_t>(std::floor(source_y));
    const auto x1 = std::min(x0 + 1U, width - 1U);
    const auto y1 = std::min(y0 + 1U, height - 1U);
    const double horizontal = static_cast<double>(source_x) - static_cast<double>(x0);
    const double vertical = static_cast<double>(source_y) - static_cast<double>(y0);
    const auto sample = [&](const std::size_t x, const std::size_t y) {
        return static_cast<double>(source[(y * width + x) * internal::rgb_channels + channel]);
    };
    const double upper = sample(x0, y0) + (sample(x1, y0) - sample(x0, y0)) * horizontal;
    const double lower = sample(x0, y1) + (sample(x1, y1) - sample(x0, y1)) * horizontal;
    const double value = upper + (lower - upper) * vertical;
    if constexpr (std::is_same_v<Sample, float>) {
        return std::isfinite(value) ? static_cast<float>(value) : 0.0F;
    } else {
        return static_cast<std::uint16_t>(std::clamp(
            std::llround(value),
            0LL,
            static_cast<long long>(std::numeric_limits<std::uint16_t>::max())
        ));
    }
}

template <typename Sample> struct ProfileCorrection final {
    OpticsProfileReceipt receipt;
    std::optional<std::vector<Sample>> samples;
};

template <typename Sample>
[[nodiscard]] ProfileCorrection<Sample> correct_profile_full(
    const LensfunModifierPlan::Impl& plan,
    const std::span<const Sample> source,
    const std::size_t row_stride_bytes,
    const lfPixelFormat pixel_format
) {
    internal::ConfiguredModifier configured = internal::configure_modifier(plan, pixel_format);
    OpticsProfileReceipt receipt = internal::receipt_for(plan, configured);
    if (!receipt.applied_distortion && !receipt.applied_tca && !receipt.applied_vignetting) {
        return {.receipt = std::move(receipt)};
    }

    internal::AlignedSamples<Sample> color_corrected(source.size());
    std::copy(source.begin(), source.end(), color_corrected.data());
    if (receipt.applied_vignetting) {
        for (std::uint32_t row = 0U; row < plan.full_dimensions.height;
             row += internal::vignetting_rows_per_batch) {
            const std::uint32_t rows =
                std::min(internal::vignetting_rows_per_batch, plan.full_dimensions.height - row);
            const std::size_t first_sample =
                static_cast<std::size_t>(row) * plan.full_dimensions.width * internal::rgb_channels;
            if (!configured.modifier->ApplyColorModification(
                    color_corrected.data() + static_cast<std::ptrdiff_t>(first_sample),
                    0.0F,
                    static_cast<float>(row),
                    static_cast<int>(plan.full_dimensions.width),
                    static_cast<int>(rows),
                    LF_CR_3(RED, GREEN, BLUE),
                    static_cast<int>(row_stride_bytes)
                )) {
                receipt.applied_vignetting = false;
                receipt.vignetting_used_distance_fallback = false;
                break;
            }
        }
    }

    std::vector<Sample> output(source.begin(), source.end());
    if (receipt.applied_distortion || receipt.applied_tca) {
        const std::uint32_t batch_rows =
            std::min(internal::remap_rows_per_batch, plan.full_dimensions.height);
        const std::size_t coordinate_capacity = internal::checked_sample_count(
            plan.full_dimensions.width,
            batch_rows,
            internal::coordinates_per_pixel,
            "Lensfun full-frame coordinate table is too large"
        );
        internal::AlignedSamples<float> coordinates(coordinate_capacity);
        const std::span<const Sample> corrected(color_corrected.data(), source.size());
        for (std::uint32_t row = 0U; row < plan.full_dimensions.height; row += batch_rows) {
            const std::uint32_t rows = std::min(batch_rows, plan.full_dimensions.height - row);
            if (!configured.modifier->ApplySubpixelGeometryDistortion(
                    0.0F,
                    static_cast<float>(row),
                    static_cast<int>(plan.full_dimensions.width),
                    static_cast<int>(rows),
                    coordinates.data()
                )) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Lensfun unexpectedly declined an enabled full-frame remap"
                );
            }
            for (std::uint32_t local_y = 0U; local_y < rows; ++local_y) {
                const std::uint32_t output_y = row + local_y;
                for (std::uint32_t x = 0U; x < plan.full_dimensions.width; ++x) {
                    const std::size_t pixel =
                        static_cast<std::size_t>(local_y) * plan.full_dimensions.width + x;
                    const std::size_t output_pixel =
                        (static_cast<std::size_t>(output_y) * plan.full_dimensions.width + x)
                        * internal::rgb_channels;
                    for (std::size_t channel = 0U; channel < internal::rgb_channels; ++channel) {
                        const std::size_t coordinate =
                            (pixel * internal::rgb_channels + channel) * 2U;
                        output[output_pixel + channel] = bilinear_sample_channel(
                            corrected,
                            plan.full_dimensions,
                            coordinates.data()[coordinate],
                            coordinates.data()[coordinate + 1U],
                            channel
                        );
                    }
                }
            }
        }
    } else if (receipt.applied_vignetting) {
        std::copy(
            color_corrected.data(),
            color_corrected.data() + static_cast<std::ptrdiff_t>(source.size()),
            output.begin()
        );
    } else {
        return {.receipt = std::move(receipt)};
    }
    return {
        .receipt = std::move(receipt),
        .samples = std::move(output),
    };
}

[[nodiscard]] float bilinear_sample_preimage_channel(
    const std::span<const float> source,
    const GeometryPixelRect source_rect,
    const Dimensions full_dimensions,
    const float absolute_x,
    const float absolute_y,
    const std::size_t channel
) {
    if (!internal::coordinate_in_full_image(absolute_x, absolute_y, full_dimensions)) {
        return 0.0F;
    }

    // Preserve the full-frame coordinate system through floor. Localizing the float first can
    // move a boundary by one ULP and creates a seam despite an otherwise exact preimage.
    const auto x0 = static_cast<std::uint32_t>(std::floor(absolute_x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(absolute_y));
    const std::uint32_t x1 = std::min(x0 + 1U, full_dimensions.width - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, full_dimensions.height - 1U);
    if (x0 < source_rect.x || y0 < source_rect.y || x1 >= source_rect.x + source_rect.width
        || y1 >= source_rect.y + source_rect.height) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "Lensfun region preimage does not contain a declared bilinear footprint"
        );
    }

    const auto local_x0 = static_cast<std::size_t>(x0 - source_rect.x);
    const auto local_y0 = static_cast<std::size_t>(y0 - source_rect.y);
    const auto local_x1 = static_cast<std::size_t>(x1 - source_rect.x);
    const auto local_y1 = static_cast<std::size_t>(y1 - source_rect.y);
    const auto source_width = static_cast<std::size_t>(source_rect.width);
    const auto sample = [&](const std::size_t x, const std::size_t y) {
        return static_cast<double>(
            source[(y * source_width + x) * internal::rgb_channels + channel]
        );
    };
    const double horizontal = static_cast<double>(absolute_x) - static_cast<double>(x0);
    const double vertical = static_cast<double>(absolute_y) - static_cast<double>(y0);
    const double upper = sample(local_x0, local_y0)
                         + (sample(local_x1, local_y0) - sample(local_x0, local_y0)) * horizontal;
    const double lower = sample(local_x0, local_y1)
                         + (sample(local_x1, local_y1) - sample(local_x0, local_y1)) * horizontal;
    const double value = upper + (lower - upper) * vertical;
    return std::isfinite(value) ? static_cast<float>(value) : 0.0F;
}

#endif

} // namespace

SceneLinearRgbFrame LensfunModifierPlan::render_scene_linear_region_cpu(
    const SceneLinearRgbFrame& source_preimage,
    const PreparedRegion& region
) const {
#if SHADOW_IMAGE_HAS_LENSFUN
    const GeometryPixelRect output_rect = region.output_rect();
    const auto& source_preimage_rect = region.source_preimage();
    const auto& absolute_source_coordinates = region.absolute_source_coordinates();
    const auto& profile_vignetting_gains = region.profile_vignetting_gains();
    internal::validate_output_rect(output_rect, implementation_->full_dimensions);
    if (region.plan_identity_.get() != region_plan_identity_.get()
        || region.executor_identity() != region_executor_identity
        || region.manual_output_settings() != implementation_->settings) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Lensfun region evidence does not belong to this concrete plan"
        );
    }

    SceneLinearRgbFrame output;
    output.dimensions = {output_rect.width, output_rect.height};
    output.row_stride_bytes =
        static_cast<std::size_t>(output_rect.width) * internal::rgb_channels * sizeof(float);
    output.samples.assign(
        internal::checked_sample_count(
            output_rect.width,
            output_rect.height,
            internal::rgb_channels,
            "Lensfun region output is too large"
        ),
        0.0F
    );
    if (!source_preimage_rect.has_value()) {
        manual_optics::apply_manual_scene_linear_vignetting_region(
            output,
            implementation_->full_dimensions,
            output_rect.x,
            output_rect.y,
            implementation_->settings
        );
        return output;
    }

    const GeometryPixelRect source_rect = *source_preimage_rect;
    if (!source_preimage.valid()
        || source_preimage.dimensions != Dimensions{source_rect.width, source_rect.height}) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Lensfun region source does not match its exact preimage"
        );
    }
    std::vector<float> source = source_preimage.samples;
    if (!profile_vignetting_gains.empty()) {
        if (profile_vignetting_gains.size() != source.size()) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Lensfun region vignetting gains do not match the source preimage"
            );
        }
        for (std::size_t index = 0U; index < source.size(); ++index) {
            const double corrected = static_cast<double>(source[index])
                                     * static_cast<double>(profile_vignetting_gains[index]);
            if (!std::isfinite(corrected)
                || corrected < -static_cast<double>(std::numeric_limits<float>::max())
                || corrected > static_cast<double>(std::numeric_limits<float>::max())) {
                throw DecodeError(
                    DecodeErrorCode::resource_limit,
                    0,
                    "Lensfun region vignetting produced a non-finite fp32 sample"
                );
            }
            // Lensfun's generic fp32 colour callback clamps every modified component at zero.
            // DCP scene-linear matrices may legitimately produce negative channel values, so
            // treating profile correction as an unconstrained gain changes textured HDR pixels
            // by whole scene-linear units even when the gain table itself is exact.
            source[index] = static_cast<float>(std::max(0.0, corrected));
        }
    }

    if (!region.coordinate_remap()) {
        if (!absolute_source_coordinates.empty() || source_rect != output_rect
            || source.size() != output.samples.size()) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Lensfun pointwise region evidence has inconsistent geometry"
            );
        }
        output.samples = std::move(source);
    } else {
        const std::size_t expected_coordinates = internal::checked_sample_count(
            output_rect.width,
            output_rect.height,
            internal::coordinates_per_pixel,
            "Lensfun region coordinate table is too large"
        );
        if (absolute_source_coordinates.size() != expected_coordinates) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Lensfun coordinate-remap evidence has an invalid table size"
            );
        }
        const std::span<const float> source_view(source);
        const std::size_t output_pixels =
            static_cast<std::size_t>(output_rect.width) * output_rect.height;
        for (std::size_t pixel = 0U; pixel < output_pixels; ++pixel) {
            for (std::size_t channel = 0U; channel < internal::rgb_channels; ++channel) {
                const std::size_t coordinate = (pixel * internal::rgb_channels + channel) * 2U;
                const float absolute_x = absolute_source_coordinates[coordinate];
                const float absolute_y = absolute_source_coordinates[coordinate + 1U];
                if (!internal::coordinate_in_full_image(
                        absolute_x,
                        absolute_y,
                        implementation_->full_dimensions
                    )) {
                    continue;
                }
                output.samples[pixel * internal::rgb_channels + channel] =
                    bilinear_sample_preimage_channel(
                        source_view,
                        source_rect,
                        implementation_->full_dimensions,
                        absolute_x,
                        absolute_y,
                        channel
                    );
            }
        }
    }

    manual_optics::apply_manual_scene_linear_vignetting_region(
        output,
        implementation_->full_dimensions,
        output_rect.x,
        output_rect.y,
        implementation_->settings
    );
    return output;
#else
    static_cast<void>(source_preimage);
    static_cast<void>(region);
    internal::throw_lensfun_unavailable();
#endif
}

OpticsCorrectionResult LensfunModifierPlan::correct_reference_rgb(const PixelBuffer& input) const {
#if SHADOW_IMAGE_HAS_LENSFUN
    validate_reference_input(input, implementation_->full_dimensions);
    auto profile = correct_profile_full<std::uint16_t>(
        *implementation_,
        input.samples,
        input.row_stride_bytes,
        LF_PF_U16
    );
    PixelBuffer profile_output;
    const PixelBuffer* manual_source = &input;
    if (profile.samples.has_value()) {
        profile_output = input;
        profile_output.samples = std::move(*profile.samples);
        manual_source = &profile_output;
    }
    if (auto manual = manual_optics::apply_manual_optics(*manual_source, implementation_->settings);
        manual.has_value()) {
        return {
            .receipt = std::move(profile.receipt),
            .corrected_reference_rgb = std::move(*manual),
        };
    }
    return {
        .receipt = std::move(profile.receipt),
        .corrected_reference_rgb = profile_output.samples.empty()
                                       ? std::optional<PixelBuffer>{}
                                       : std::optional<PixelBuffer>{std::move(profile_output)},
    };
#else
    static_cast<void>(input);
    internal::throw_lensfun_unavailable();
#endif
}

SceneLinearOpticsCorrectionResult
LensfunModifierPlan::correct_scene_linear_reference(const SceneLinearRgbFrame& input) const {
#if SHADOW_IMAGE_HAS_LENSFUN
    manual_optics::validate_manual_scene_linear_input(input);
    if (input.dimensions != implementation_->full_dimensions) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Lensfun scene-linear input dimensions do not match the prepared plan"
        );
    }
    auto profile = correct_profile_full<float>(
        *implementation_,
        input.samples,
        input.row_stride_bytes,
        LF_PF_F32
    );
    SceneLinearRgbFrame profile_output;
    const SceneLinearRgbFrame* manual_source = &input;
    if (profile.samples.has_value()) {
        profile_output = input;
        profile_output.samples = std::move(*profile.samples);
        manual_source = &profile_output;
    }
    if (auto manual = manual_optics::apply_manual_optics(*manual_source, implementation_->settings);
        manual.has_value()) {
        return {
            .receipt = std::move(profile.receipt),
            .corrected_scene_linear_rgb = std::move(*manual),
        };
    }
    return {
        .receipt = std::move(profile.receipt),
        .corrected_scene_linear_rgb =
            profile_output.samples.empty()
                ? std::optional<SceneLinearRgbFrame>{}
                : std::optional<SceneLinearRgbFrame>{std::move(profile_output)},
    };
#else
    static_cast<void>(input);
    internal::throw_lensfun_unavailable();
#endif
}

} // namespace shadow::image::detail::lensfun_modifier_plan
