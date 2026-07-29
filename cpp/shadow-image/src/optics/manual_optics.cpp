#include "manual_optics.hpp"

#include <shadow/image/decoder_error.hpp>

#include "../acceleration/image_acceleration_policy.hpp"
#include "metal_manual_optics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace shadow::image::detail::manual_optics {

constexpr std::size_t rgb_channels = 3U;
constexpr std::int16_t manual_optics_limit = 100;

void validate_settings(const OpticsSettings& settings) {
    if (settings.schema_version != optics_settings_schema_version) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "unsupported optics settings schema version"
        );
    }
    const bool has_manual_camera = !settings.camera_profile_model.empty();
    const bool has_manual_lens = !settings.lens_profile_model.empty();
    if (has_manual_camera != has_manual_lens) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "manual optics selection requires both camera and lens profile models"
        );
    }
    for (const auto value : {
             settings.manual_distortion,
             settings.manual_tca_red_cyan,
             settings.manual_tca_blue_yellow,
             settings.manual_vignetting_amount,
         }) {
        if (value < -manual_optics_limit || value > manual_optics_limit) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "manual optics correction is outside the supported [-100, 100] range"
            );
        }
    }
    if (settings.manual_vignetting_midpoint > 100U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "manual optical-vignetting midpoint is outside the supported [0, 100] range"
        );
    }
}

namespace {

[[nodiscard]] bool has_manual_optics(const OpticsSettings& settings) noexcept {
    return settings.manual_distortion != 0 || settings.manual_tca_red_cyan != 0
           || settings.manual_tca_blue_yellow != 0 || settings.manual_vignetting_amount != 0;
}

void validate_manual_input(const PixelBuffer& input) {
    if (input.dimensions.width == 0U || input.dimensions.height == 0U
        || input.bits_per_channel != 16U || input.channels != rgb_channels
        || input.transfer_function != RgbTransferFunction::linear
        || input.primaries != RgbPrimaries::srgb_rec709_d65
        || (input.reference != RgbBufferReference::processed_raw
            && input.reference != RgbBufferReference::decoded_raster)) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "manual optics expects a linear processed 16-bit sRGB-primary RGB reference"
        );
    }
    const auto width = static_cast<std::size_t>(input.dimensions.width);
    const auto height = static_cast<std::size_t>(input.dimensions.height);
    if (width > std::numeric_limits<std::size_t>::max() / rgb_channels
        || height > std::numeric_limits<std::size_t>::max() / (width * rgb_channels)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "manual optics input dimensions overflow the address space"
        );
    }
    const auto expected_samples = width * height * rgb_channels;
    if (input.row_stride_bytes != width * rgb_channels * sizeof(std::uint16_t)
        || input.samples.size() != expected_samples) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "manual optics input layout does not match its RGB descriptor"
        );
    }
}

[[nodiscard]] std::uint16_t manual_bilinear_sample_channel(
    const std::vector<std::uint16_t>& source,
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
        return 0U;
    }
    const auto x0 = static_cast<std::size_t>(std::floor(source_x));
    const auto y0 = static_cast<std::size_t>(std::floor(source_y));
    const auto x1 = std::min(x0 + 1U, width - 1U);
    const auto y1 = std::min(y0 + 1U, height - 1U);
    const auto horizontal = static_cast<double>(source_x) - static_cast<double>(x0);
    const auto vertical = static_cast<double>(source_y) - static_cast<double>(y0);
    const auto sample = [&](const std::size_t x, const std::size_t y) {
        return static_cast<double>(source[(y * width + x) * rgb_channels + channel]);
    };
    const auto upper = sample(x0, y0) + (sample(x1, y0) - sample(x0, y0)) * horizontal;
    const auto lower = sample(x0, y1) + (sample(x1, y1) - sample(x0, y1)) * horizontal;
    const auto value = upper + (lower - upper) * vertical;
    return static_cast<std::uint16_t>(std::clamp(
        std::llround(value),
        0LL,
        static_cast<long long>(std::numeric_limits<std::uint16_t>::max())
    ));
}

} // namespace

bool has_manual_geometry(const OpticsSettings& settings) noexcept {
    return settings.manual_distortion != 0 || settings.manual_tca_red_cyan != 0
           || settings.manual_tca_blue_yellow != 0;
}

bool has_manual_vignetting(const OpticsSettings& settings) noexcept {
    return settings.manual_vignetting_amount != 0;
}

[[nodiscard]] std::optional<PixelBuffer>
apply_manual_optics(const PixelBuffer& input, const OpticsSettings& settings) {
    if (!has_manual_optics(settings))
        return std::nullopt;
    validate_manual_input(input);

    PixelBuffer output = input;
    const auto width = static_cast<std::size_t>(input.dimensions.width);
    const auto height = static_cast<std::size_t>(input.dimensions.height);
    const bool remap = has_manual_geometry(settings);
    const double center_x = (static_cast<double>(width) - 1.0) * 0.5;
    const double center_y = (static_cast<double>(height) - 1.0) * 0.5;
    const double radius_scale = std::hypot(center_x, center_y);
    const double distortion = static_cast<double>(settings.manual_distortion) * 0.0022;
    // A positive residual samples farther from the optical center. Crop just
    // enough to keep that radial expansion inside the source frame when the
    // photographer has asked for automatic crop.
    const double crop_scale = settings.automatic_scale && distortion > 0.0 ? 1.0 + distortion : 1.0;
    const double red_scale = 1.0 + static_cast<double>(settings.manual_tca_red_cyan) * 0.00055;
    const double blue_scale = 1.0 + static_cast<double>(settings.manual_tca_blue_yellow) * 0.00055;
    const double vignette_amount = static_cast<double>(settings.manual_vignetting_amount) / 100.0;
    const double vignette_midpoint =
        static_cast<double>(settings.manual_vignetting_midpoint) / 100.0;

    const auto corrected_sample =
        [&](const double source_x, const double source_y, const std::size_t channel) {
            return manual_bilinear_sample_channel(
                input.samples,
                input.dimensions,
                static_cast<float>(source_x),
                static_cast<float>(source_y),
                channel
            );
        };
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            const auto output_index = (y * width + x) * rgb_channels;
            const double normalized_x =
                (static_cast<double>(x) - center_x) / (radius_scale * crop_scale);
            const double normalized_y =
                (static_cast<double>(y) - center_y) / (radius_scale * crop_scale);
            const double radius_squared = normalized_x * normalized_x + normalized_y * normalized_y;
            const double radial_scale = 1.0 + distortion * radius_squared;
            const auto source_coordinate = [&](const double chromatic_scale) {
                return std::pair{
                    center_x + normalized_x * radial_scale * chromatic_scale * radius_scale,
                    center_y + normalized_y * radial_scale * chromatic_scale * radius_scale,
                };
            };
            const auto red = source_coordinate(red_scale);
            const auto green = source_coordinate(1.0);
            const auto blue = source_coordinate(blue_scale);
            const std::array coordinates{red, green, blue};
            double vignette_gain = 1.0;
            if (vignette_amount != 0.0) {
                const double radius = std::min(1.0, std::sqrt(radius_squared));
                const double denominator = std::max(1e-6, 1.0 - vignette_midpoint);
                const double progress =
                    std::clamp((radius - vignette_midpoint) / denominator, 0.0, 1.0);
                const double feathered = progress * progress * (3.0 - 2.0 * progress);
                vignette_gain = std::exp2(vignette_amount * feathered * 1.15);
            }
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                const auto [source_x, source_y] = coordinates[channel];
                const auto source_value = remap ? corrected_sample(source_x, source_y, channel)
                                                : input.samples[output_index + channel];
                output.samples[output_index + channel] = static_cast<std::uint16_t>(std::clamp(
                    std::llround(static_cast<double>(source_value) * vignette_gain),
                    0LL,
                    static_cast<long long>(std::numeric_limits<std::uint16_t>::max())
                ));
            }
        }
    }
    return output;
}

[[nodiscard]] OpticsCorrectionResult with_manual_optics(
    OpticsProfileReceipt receipt,
    const PixelBuffer& input,
    const OpticsSettings& settings
) {
    return OpticsCorrectionResult{
        .receipt = std::move(receipt),
        .corrected_reference_rgb = apply_manual_optics(input, settings),
    };
}

void validate_manual_scene_linear_input(const SceneLinearRgbFrame& input) {
    if (!input.valid() || input.dimensions.width == 0U || input.dimensions.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "manual optics expects a valid scene-linear fp32 RGB reference"
        );
    }
    const auto width = static_cast<std::size_t>(input.dimensions.width);
    const auto height = static_cast<std::size_t>(input.dimensions.height);
    if (width > std::numeric_limits<std::size_t>::max() / rgb_channels
        || height > std::numeric_limits<std::size_t>::max() / (width * rgb_channels)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "manual scene-linear optics input dimensions overflow the address space"
        );
    }
    const auto expected_samples = width * height * rgb_channels;
    if (input.row_stride_bytes != width * rgb_channels * sizeof(float)
        || input.samples.size() != expected_samples) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "manual scene-linear optics input layout does not match its RGB descriptor"
        );
    }
}

void apply_manual_scene_linear_vignetting_region(
    SceneLinearRgbFrame& input,
    const Dimensions full_dimensions,
    const std::uint32_t origin_x,
    const std::uint32_t origin_y,
    const OpticsSettings& settings
) {
    validate_settings(settings);
    validate_manual_scene_linear_input(input);
    if (has_manual_geometry(settings)) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "pointwise region optics cannot execute manual geometry"
        );
    }
    if (full_dimensions.width == 0U || full_dimensions.height == 0U
        || origin_x >= full_dimensions.width || origin_y >= full_dimensions.height
        || input.dimensions.width > full_dimensions.width - origin_x
        || input.dimensions.height > full_dimensions.height - origin_y) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "manual vignette region is outside the full image"
        );
    }
    if (!has_manual_vignetting(settings)) {
        return;
    }

    const double center_x = (static_cast<double>(full_dimensions.width) - 1.0) * 0.5;
    const double center_y = (static_cast<double>(full_dimensions.height) - 1.0) * 0.5;
    const double radius_scale = std::max(1.0, std::hypot(center_x, center_y));
    const double vignette_amount = static_cast<double>(settings.manual_vignetting_amount) / 100.0;
    const double vignette_midpoint =
        static_cast<double>(settings.manual_vignetting_midpoint) / 100.0;
    for (std::uint32_t local_y = 0U; local_y < input.dimensions.height; ++local_y) {
        const double global_y = static_cast<double>(origin_y + local_y);
        for (std::uint32_t local_x = 0U; local_x < input.dimensions.width; ++local_x) {
            const double global_x = static_cast<double>(origin_x + local_x);
            const double normalized_x = (global_x - center_x) / radius_scale;
            const double normalized_y = (global_y - center_y) / radius_scale;
            const double radius =
                std::min(1.0, std::sqrt(normalized_x * normalized_x + normalized_y * normalized_y));
            const double denominator = std::max(1e-6, 1.0 - vignette_midpoint);
            const double progress =
                std::clamp((radius - vignette_midpoint) / denominator, 0.0, 1.0);
            const double feathered = progress * progress * (3.0 - 2.0 * progress);
            const double gain = std::exp2(vignette_amount * feathered * 1.15);
            const std::size_t index =
                (static_cast<std::size_t>(local_y) * input.dimensions.width + local_x)
                * rgb_channels;
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                const double corrected = static_cast<double>(input.samples[index + channel]) * gain;
                if (!std::isfinite(corrected)
                    || corrected < -static_cast<double>(std::numeric_limits<float>::max())
                    || corrected > static_cast<double>(std::numeric_limits<float>::max())) {
                    throw DecodeError(
                        DecodeErrorCode::resource_limit,
                        0,
                        "manual scene-linear vignette produced a non-finite fp32 sample"
                    );
                }
                input.samples[index + channel] = static_cast<float>(corrected);
            }
        }
    }
}

namespace {

[[nodiscard]] float manual_bilinear_scene_linear_sample_channel(
    const std::vector<float>& source,
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
        return 0.0F;
    }
    const auto x0 = static_cast<std::size_t>(std::floor(source_x));
    const auto y0 = static_cast<std::size_t>(std::floor(source_y));
    const auto x1 = std::min(x0 + 1U, width - 1U);
    const auto y1 = std::min(y0 + 1U, height - 1U);
    const auto horizontal = static_cast<double>(source_x) - static_cast<double>(x0);
    const auto vertical = static_cast<double>(source_y) - static_cast<double>(y0);
    const auto sample = [&](const std::size_t x, const std::size_t y) {
        return static_cast<double>(source[(y * width + x) * rgb_channels + channel]);
    };
    const double upper = sample(x0, y0) + (sample(x1, y0) - sample(x0, y0)) * horizontal;
    const double lower = sample(x0, y1) + (sample(x1, y1) - sample(x0, y1)) * horizontal;
    const double value = upper + (lower - upper) * vertical;
    return std::isfinite(value) ? static_cast<float>(value) : 0.0F;
}

} // namespace

[[nodiscard]] std::optional<SceneLinearRgbFrame>
apply_manual_optics(const SceneLinearRgbFrame& input, const OpticsSettings& settings) {
    if (!has_manual_optics(settings))
        return std::nullopt;
    validate_manual_scene_linear_input(input);

    const auto acceleration = image_acceleration_preference_from_environment();
    if (!acceleration.has_value()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "SHADOW_IMAGE_ACCELERATION must be auto, cpu, or metal"
        );
    }
    if (*acceleration != ImageAccelerationPreference::cpu) {
        auto metal = try_apply_manual_scene_linear_optics_metal(input, settings);
        if (metal.corrected.has_value()) {
            return std::move(metal.corrected);
        }
        if (*acceleration == ImageAccelerationPreference::metal) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                metal.diagnostic.empty() ? "Metal manual scene-linear optics is unavailable"
                                         : std::move(metal.diagnostic)
            );
        }
    }

    SceneLinearRgbFrame output = input;
    if (!has_manual_geometry(settings)) {
        apply_manual_scene_linear_vignetting_region(output, input.dimensions, 0U, 0U, settings);
        return output;
    }
    const auto width = static_cast<std::size_t>(input.dimensions.width);
    const auto height = static_cast<std::size_t>(input.dimensions.height);
    const bool remap = has_manual_geometry(settings);
    const double center_x = (static_cast<double>(width) - 1.0) * 0.5;
    const double center_y = (static_cast<double>(height) - 1.0) * 0.5;
    const double radius_scale = std::max(1.0, std::hypot(center_x, center_y));
    const double distortion = static_cast<double>(settings.manual_distortion) * 0.0022;
    const double crop_scale = settings.automatic_scale && distortion > 0.0 ? 1.0 + distortion : 1.0;
    const double red_scale = 1.0 + static_cast<double>(settings.manual_tca_red_cyan) * 0.00055;
    const double blue_scale = 1.0 + static_cast<double>(settings.manual_tca_blue_yellow) * 0.00055;
    const double vignette_amount = static_cast<double>(settings.manual_vignetting_amount) / 100.0;
    const double vignette_midpoint =
        static_cast<double>(settings.manual_vignetting_midpoint) / 100.0;

    const auto corrected_sample =
        [&](const double source_x, const double source_y, const std::size_t channel) {
            return manual_bilinear_scene_linear_sample_channel(
                input.samples,
                input.dimensions,
                static_cast<float>(source_x),
                static_cast<float>(source_y),
                channel
            );
        };
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            const auto output_index = (y * width + x) * rgb_channels;
            const double normalized_x =
                (static_cast<double>(x) - center_x) / (radius_scale * crop_scale);
            const double normalized_y =
                (static_cast<double>(y) - center_y) / (radius_scale * crop_scale);
            const double radius_squared = normalized_x * normalized_x + normalized_y * normalized_y;
            const double radial_scale = 1.0 + distortion * radius_squared;
            const auto source_coordinate = [&](const double chromatic_scale) {
                return std::pair{
                    center_x + normalized_x * radial_scale * chromatic_scale * radius_scale,
                    center_y + normalized_y * radial_scale * chromatic_scale * radius_scale,
                };
            };
            const std::array coordinates{
                source_coordinate(red_scale),
                source_coordinate(1.0),
                source_coordinate(blue_scale)
            };
            double vignette_gain = 1.0;
            if (vignette_amount != 0.0) {
                const double radius = std::min(1.0, std::sqrt(radius_squared));
                const double denominator = std::max(1e-6, 1.0 - vignette_midpoint);
                const double progress =
                    std::clamp((radius - vignette_midpoint) / denominator, 0.0, 1.0);
                const double feathered = progress * progress * (3.0 - 2.0 * progress);
                vignette_gain = std::exp2(vignette_amount * feathered * 1.15);
            }
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                const auto [source_x, source_y] = coordinates[channel];
                const double source_value =
                    remap ? static_cast<double>(corrected_sample(source_x, source_y, channel))
                          : static_cast<double>(input.samples[output_index + channel]);
                const double corrected = source_value * vignette_gain;
                if (!std::isfinite(corrected)
                    || corrected < -static_cast<double>(std::numeric_limits<float>::max())
                    || corrected > static_cast<double>(std::numeric_limits<float>::max())) {
                    throw DecodeError(
                        DecodeErrorCode::resource_limit,
                        0,
                        "manual scene-linear optics produced a non-finite fp32 sample"
                    );
                }
                output.samples[output_index + channel] = static_cast<float>(corrected);
            }
        }
    }
    return output;
}

[[nodiscard]] SceneLinearOpticsCorrectionResult with_manual_optics(
    OpticsProfileReceipt receipt,
    const SceneLinearRgbFrame& input,
    const OpticsSettings& settings
) {
    return SceneLinearOpticsCorrectionResult{
        .receipt = std::move(receipt),
        .corrected_scene_linear_rgb = apply_manual_optics(input, settings),
    };
}

} // namespace shadow::image::detail::manual_optics
