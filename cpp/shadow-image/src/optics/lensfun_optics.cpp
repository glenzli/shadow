#include <shadow/image/optics.hpp>

#include <shadow/image/decoder_error.hpp>

#include "lensfun_profile_catalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef SHADOW_IMAGE_HAS_LENSFUN
#define SHADOW_IMAGE_HAS_LENSFUN 0
#endif

#if SHADOW_IMAGE_HAS_LENSFUN
#include <lensfun/lensfun.h>
#endif

namespace shadow::image {

namespace {

constexpr std::size_t rgb_channels = 3U;
constexpr std::int16_t manual_optics_limit = 100;

[[nodiscard]] OpticsProfileReceipt unavailable_receipt(
    const OpticsProviderInfo& info,
    const OpticsProfileStatus status
) {
    return OpticsProfileReceipt{
        .status = status,
        .provider_id = info.id,
        .provider_version = info.version,
    };
}

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

[[nodiscard]] bool has_manual_optics(const OpticsSettings& settings) noexcept {
    return settings.manual_distortion != 0
        || settings.manual_tca_red_cyan != 0
        || settings.manual_tca_blue_yellow != 0
        || settings.manual_vignetting_amount != 0;
}

[[nodiscard]] bool has_manual_geometry(const OpticsSettings& settings) noexcept {
    return settings.manual_distortion != 0
        || settings.manual_tca_red_cyan != 0
        || settings.manual_tca_blue_yellow != 0;
}

void validate_manual_input(const PixelBuffer& input) {
    if (
        input.dimensions.width == 0U || input.dimensions.height == 0U
        || input.bits_per_channel != 16U || input.channels != rgb_channels
        || input.transfer_function != RgbTransferFunction::linear
        || input.primaries != RgbPrimaries::srgb_rec709_d65
        || (input.reference != RgbBufferReference::processed_raw
            && input.reference != RgbBufferReference::decoded_raster)
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "manual optics expects a linear processed 16-bit sRGB-primary RGB reference"
        );
    }
    const auto width = static_cast<std::size_t>(input.dimensions.width);
    const auto height = static_cast<std::size_t>(input.dimensions.height);
    if (
        width > std::numeric_limits<std::size_t>::max() / rgb_channels
        || height > std::numeric_limits<std::size_t>::max() / (width * rgb_channels)
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "manual optics input dimensions overflow the address space"
        );
    }
    const auto expected_samples = width * height * rgb_channels;
    if (
        input.row_stride_bytes != width * rgb_channels * sizeof(std::uint16_t)
        || input.samples.size() != expected_samples
    ) {
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
    if (
        !std::isfinite(source_x) || !std::isfinite(source_y) || source_x < 0.0F
        || source_y < 0.0F || source_x > static_cast<float>(dimensions.width - 1U)
        || source_y > static_cast<float>(dimensions.height - 1U)
    ) {
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

[[nodiscard]] std::optional<PixelBuffer> apply_manual_optics(
    const PixelBuffer& input,
    const OpticsSettings& settings
) {
    if (!has_manual_optics(settings)) return std::nullopt;
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
    const double crop_scale = settings.automatic_scale && distortion > 0.0
        ? 1.0 + distortion : 1.0;
    const double red_scale = 1.0
        + static_cast<double>(settings.manual_tca_red_cyan) * 0.00055;
    const double blue_scale = 1.0
        + static_cast<double>(settings.manual_tca_blue_yellow) * 0.00055;
    const double vignette_amount =
        static_cast<double>(settings.manual_vignetting_amount) / 100.0;
    const double vignette_midpoint =
        static_cast<double>(settings.manual_vignetting_midpoint) / 100.0;

    const auto corrected_sample = [&](const double source_x, const double source_y,
                                      const std::size_t channel) {
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
            const double normalized_x = (static_cast<double>(x) - center_x)
                / (radius_scale * crop_scale);
            const double normalized_y = (static_cast<double>(y) - center_y)
                / (radius_scale * crop_scale);
            const double radius_squared = normalized_x * normalized_x
                + normalized_y * normalized_y;
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
                const double progress = std::clamp(
                    (radius - vignette_midpoint) / denominator, 0.0, 1.0
                );
                const double feathered = progress * progress * (3.0 - 2.0 * progress);
                vignette_gain = std::exp2(vignette_amount * feathered * 1.15);
            }
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                const auto [source_x, source_y] = coordinates[channel];
                const auto source_value = remap
                    ? corrected_sample(source_x, source_y, channel)
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
    if (
        width > std::numeric_limits<std::size_t>::max() / rgb_channels
        || height > std::numeric_limits<std::size_t>::max() / (width * rgb_channels)
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "manual scene-linear optics input dimensions overflow the address space"
        );
    }
    const auto expected_samples = width * height * rgb_channels;
    if (
        input.row_stride_bytes != width * rgb_channels * sizeof(float)
        || input.samples.size() != expected_samples
    ) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "manual scene-linear optics input layout does not match its RGB descriptor"
        );
    }
}

[[nodiscard]] float manual_bilinear_scene_linear_sample_channel(
    const std::vector<float>& source,
    const Dimensions dimensions,
    const float source_x,
    const float source_y,
    const std::size_t channel
) noexcept {
    const auto width = static_cast<std::size_t>(dimensions.width);
    const auto height = static_cast<std::size_t>(dimensions.height);
    if (
        !std::isfinite(source_x) || !std::isfinite(source_y) || source_x < 0.0F
        || source_y < 0.0F || source_x > static_cast<float>(dimensions.width - 1U)
        || source_y > static_cast<float>(dimensions.height - 1U)
    ) {
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

[[nodiscard]] std::optional<SceneLinearRgbFrame> apply_manual_optics(
    const SceneLinearRgbFrame& input,
    const OpticsSettings& settings
) {
    if (!has_manual_optics(settings)) return std::nullopt;
    validate_manual_scene_linear_input(input);

    SceneLinearRgbFrame output = input;
    const auto width = static_cast<std::size_t>(input.dimensions.width);
    const auto height = static_cast<std::size_t>(input.dimensions.height);
    const bool remap = has_manual_geometry(settings);
    const double center_x = (static_cast<double>(width) - 1.0) * 0.5;
    const double center_y = (static_cast<double>(height) - 1.0) * 0.5;
    const double radius_scale = std::max(1.0, std::hypot(center_x, center_y));
    const double distortion = static_cast<double>(settings.manual_distortion) * 0.0022;
    const double crop_scale = settings.automatic_scale && distortion > 0.0
        ? 1.0 + distortion : 1.0;
    const double red_scale = 1.0
        + static_cast<double>(settings.manual_tca_red_cyan) * 0.00055;
    const double blue_scale = 1.0
        + static_cast<double>(settings.manual_tca_blue_yellow) * 0.00055;
    const double vignette_amount =
        static_cast<double>(settings.manual_vignetting_amount) / 100.0;
    const double vignette_midpoint =
        static_cast<double>(settings.manual_vignetting_midpoint) / 100.0;

    const auto corrected_sample = [&](const double source_x, const double source_y,
                                      const std::size_t channel) {
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
            const double normalized_x = (static_cast<double>(x) - center_x)
                / (radius_scale * crop_scale);
            const double normalized_y = (static_cast<double>(y) - center_y)
                / (radius_scale * crop_scale);
            const double radius_squared = normalized_x * normalized_x
                + normalized_y * normalized_y;
            const double radial_scale = 1.0 + distortion * radius_squared;
            const auto source_coordinate = [&](const double chromatic_scale) {
                return std::pair{
                    center_x + normalized_x * radial_scale * chromatic_scale * radius_scale,
                    center_y + normalized_y * radial_scale * chromatic_scale * radius_scale,
                };
            };
            const std::array coordinates{
                source_coordinate(red_scale), source_coordinate(1.0), source_coordinate(blue_scale)
            };
            double vignette_gain = 1.0;
            if (vignette_amount != 0.0) {
                const double radius = std::min(1.0, std::sqrt(radius_squared));
                const double denominator = std::max(1e-6, 1.0 - vignette_midpoint);
                const double progress = std::clamp(
                    (radius - vignette_midpoint) / denominator, 0.0, 1.0
                );
                const double feathered = progress * progress * (3.0 - 2.0 * progress);
                vignette_gain = std::exp2(vignette_amount * feathered * 1.15);
            }
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                const auto [source_x, source_y] = coordinates[channel];
                const double source_value = remap
                    ? static_cast<double>(corrected_sample(source_x, source_y, channel))
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

#if SHADOW_IMAGE_HAS_LENSFUN

constexpr std::uint32_t remap_rows_per_batch = 48U;

[[nodiscard]] bool finite_positive(const double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

void validate_input(const PixelBuffer& input) {
    if (
        input.dimensions.width == 0U || input.dimensions.height == 0U
        || input.bits_per_channel != 16U || input.channels != rgb_channels
        || input.transfer_function != RgbTransferFunction::linear
        || input.primaries != RgbPrimaries::srgb_rec709_d65
        || input.reference != RgbBufferReference::processed_raw
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "optics expects linear processed 16-bit sRGB-primary RGB"
        );
    }
    const auto width = static_cast<std::size_t>(input.dimensions.width);
    const auto height = static_cast<std::size_t>(input.dimensions.height);
    if (
        width > std::numeric_limits<std::size_t>::max() / rgb_channels
        || height > std::numeric_limits<std::size_t>::max() / (width * rgb_channels)
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "optics input dimensions overflow the address space"
        );
    }
    const auto expected_samples = width * height * rgb_channels;
    if (
        input.row_stride_bytes != width * rgb_channels * sizeof(std::uint16_t)
        || input.samples.size() != expected_samples
    ) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "optics input layout does not match its RGB descriptor"
        );
    }
}

template <typename Sample>
class AlignedSamples final {
public:
    explicit AlignedSamples(const std::size_t count) : count_(count) {
        if (count_ == 0U || count_ > std::numeric_limits<std::size_t>::max() / sizeof(Sample)) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "optics aligned working buffer is too large"
            );
        }
        data_ = static_cast<Sample*>(
            ::operator new(count_ * sizeof(Sample), std::align_val_t{16U})
        );
    }

    AlignedSamples(const AlignedSamples&) = delete;
    AlignedSamples& operator=(const AlignedSamples&) = delete;

    ~AlignedSamples() {
        ::operator delete(data_, std::align_val_t{16U});
    }

    [[nodiscard]] Sample* data() noexcept {
        return data_;
    }

private:
    Sample* data_ = nullptr;
    std::size_t count_ = 0U;
};

[[nodiscard]] std::uint16_t bilinear_sample_channel(
    const std::span<const std::uint16_t> source,
    const Dimensions dimensions,
    const float source_x,
    const float source_y,
    const std::size_t channel
) noexcept {
    const auto width = static_cast<std::size_t>(dimensions.width);
    const auto height = static_cast<std::size_t>(dimensions.height);
    if (
        !std::isfinite(source_x) || !std::isfinite(source_y) || source_x < 0.0F
        || source_y < 0.0F || source_x > static_cast<float>(dimensions.width - 1U)
        || source_y > static_cast<float>(dimensions.height - 1U)
    ) {
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

[[nodiscard]] float bilinear_scene_linear_sample_channel(
    const std::span<const float> source,
    const Dimensions dimensions,
    const float source_x,
    const float source_y,
    const std::size_t channel
) noexcept {
    const auto width = static_cast<std::size_t>(dimensions.width);
    const auto height = static_cast<std::size_t>(dimensions.height);
    if (
        !std::isfinite(source_x) || !std::isfinite(source_y) || source_x < 0.0F
        || source_y < 0.0F || source_x > static_cast<float>(dimensions.width - 1U)
        || source_y > static_cast<float>(dimensions.height - 1U)
    ) {
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

[[nodiscard]] float crop_factor(const AssetMetadata& metadata, const lfCamera& camera) noexcept {
    if (
        finite_positive(metadata.focal_length_mm) && finite_positive(metadata.focal_length_35mm)
    ) {
        const auto derived = metadata.focal_length_35mm / metadata.focal_length_mm;
        if (std::isfinite(derived) && derived >= 0.5 && derived <= 8.0) {
            return static_cast<float>(derived);
        }
    }
    return camera.CropFactor > 0.0F ? camera.CropFactor : 1.0F;
}

class LensfunOpticsProvider final : public OpticsProvider {
public:
    explicit LensfunOpticsProvider(std::optional<std::filesystem::path> database_directory)
        : profile_catalog_(std::move(database_directory)) {}

    [[nodiscard]] const OpticsProviderInfo& info() const noexcept override {
        return profile_catalog_.info();
    }

    [[nodiscard]] std::vector<OpticsProfileCandidate> profile_candidates(
        const AssetMetadata& metadata
    ) const override {
        return profile_catalog_.profile_candidates(metadata);
    }

    [[nodiscard]] OpticsCorrectionResult correct_reference_rgb(
        const PixelBuffer& input,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const override {
        validate_settings(settings);
        if (!settings.enabled) {
            return with_manual_optics(
                unavailable_receipt(
                    profile_catalog_.info(), OpticsProfileStatus::disabled
                ),
                input,
                settings
            );
        }
        if (!profile_catalog_.info().available) {
            return with_manual_optics(
                unavailable_receipt(
                    profile_catalog_.info(),
                    OpticsProfileStatus::provider_unavailable
                ),
                input,
                settings
            );
        }
        if (
            !finite_positive(metadata.focal_length_mm)
            || !profile_catalog_.profile_identity_available(metadata, settings)
        ) {
            return with_manual_optics(
                unavailable_receipt(
                    profile_catalog_.info(),
                    OpticsProfileStatus::insufficient_metadata
                ),
                input,
                settings
            );
        }
        if (
            input.bits_per_channel != 16U || input.channels != rgb_channels
            || input.transfer_function != RgbTransferFunction::linear
            || input.primaries != RgbPrimaries::srgb_rec709_d65
            || input.reference != RgbBufferReference::processed_raw
        ) {
            return with_manual_optics(
                unavailable_receipt(
                    profile_catalog_.info(),
                    OpticsProfileStatus::incompatible_input
                ),
                input,
                settings
            );
        }
        validate_input(input);

        const auto resolution = profile_catalog_.resolve_profile(metadata, settings);
        if (!resolution.match.has_value()) {
            return with_manual_optics(
                unavailable_receipt(profile_catalog_.info(), resolution.status),
                input,
                settings
            );
        }
        const auto& match = *resolution.match;

        const auto width = input.dimensions.width;
        const auto height = input.dimensions.height;
        if (
            width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            || height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        ) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "optics image dimensions exceed Lensfun's integer API"
            );
        }

        // LibRaw does not expose a broadly reliable focus-distance field. Lightroom-like
        // workflows nevertheless apply the ordinary lens profile in this situation: distance
        // matters chiefly for close-focus corrections, while the far-distance calibration is
        // normally the useful default. Use an explicit 1 km approximation and report it in the
        // receipt instead of turning vignetting off for almost every LibRaw asset.
        const bool has_vignetting_aperture = finite_positive(metadata.aperture_f_number);
        const bool has_vignetting_distance = finite_positive(metadata.focus_distance_meters);
        const bool request_vignetting = settings.correct_vignetting && has_vignetting_aperture;
        const float vignetting_distance_meters = has_vignetting_distance
            ? static_cast<float>(metadata.focus_distance_meters)
            : 1000.0F;
        const bool vignetting_uses_distance_fallback = request_vignetting
            && !has_vignetting_distance;

#if LF_VERSION_MICRO >= 99
        // Lensfun 0.3.99+ replaced Initialize() with focused enable calls. Keep the adapter on
        // the native API of each supported line instead of emulating one through deprecated C
        // wrappers; Homebrew currently ships stable 0.3.4 while upstream development uses 0.3.99.
        lfModifier modifier(
            match.lens,
            static_cast<float>(metadata.focal_length_mm),
            crop_factor(metadata, *match.camera),
            static_cast<int>(width),
            static_cast<int>(height),
            LF_PF_U16
        );
        if (settings.correct_distortion) {
            modifier.EnableDistortionCorrection();
        }
        if (settings.correct_tca) {
            modifier.EnableTCACorrection();
        }
        if (request_vignetting) {
            modifier.EnableVignettingCorrection(
                static_cast<float>(metadata.aperture_f_number),
                vignetting_distance_meters
            );
        }

        auto flags = modifier.GetModFlags();
        const auto geometry_requested = (flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) != 0;
        bool applied_scaling = false;
        if (settings.automatic_scale && geometry_requested) {
            const auto automatic_scale = modifier.GetAutoScale(false);
            if (std::isfinite(automatic_scale) && automatic_scale > 0.0F) {
                modifier.EnableScaling(automatic_scale);
                flags = modifier.GetModFlags();
                applied_scaling = (flags & LF_MODIFY_SCALE) != 0;
            }
        }
#else
        // Lensfun 0.3.4 configures all requested corrections in one Initialize() call. A scale
        // of zero asks Lensfun to compute its calibrated automatic scale. At normal focus
        // distances, 1000 m is our documented fallback for a missing portable focus field;
        // when vignetting is not requested it remains an inert API placeholder.
        int requested_flags = 0;
        if (settings.correct_distortion) {
            requested_flags |= LF_MODIFY_DISTORTION;
        }
        if (settings.correct_tca) {
            requested_flags |= LF_MODIFY_TCA;
        }
        if (request_vignetting) {
            requested_flags |= LF_MODIFY_VIGNETTING;
        }
        const auto geometry_requested =
            (requested_flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) != 0;
        if (settings.automatic_scale && geometry_requested) {
            requested_flags |= LF_MODIFY_SCALE;
        }

        lfModifier modifier(
            match.lens,
            crop_factor(metadata, *match.camera),
            static_cast<int>(width),
            static_cast<int>(height)
        );
        const auto flags = modifier.Initialize(
            match.lens,
            LF_PF_U16,
            static_cast<float>(metadata.focal_length_mm),
            request_vignetting ? static_cast<float>(metadata.aperture_f_number) : 0.0F,
            request_vignetting ? vignetting_distance_meters : 1000.0F,
            settings.automatic_scale && geometry_requested ? 0.0F : 1.0F,
            match.lens->Type,
            requested_flags,
            false
        );
        const bool applied_scaling = (flags & LF_MODIFY_SCALE) != 0;
#endif

        OpticsProfileReceipt receipt{
            .status = OpticsProfileStatus::matched,
            .provider_id = profile_catalog_.info().id,
            .provider_version = profile_catalog_.info().version,
            .camera_profile = match.camera_name,
            .lens_profile = match.lens_name,
            .distortion_available = (flags & LF_MODIFY_DISTORTION) != 0,
            .tca_available = (flags & LF_MODIFY_TCA) != 0,
            .vignetting_available = (flags & LF_MODIFY_VIGNETTING) != 0,
            .applied_distortion = (flags & LF_MODIFY_DISTORTION) != 0,
            .applied_tca = (flags & LF_MODIFY_TCA) != 0,
            .applied_vignetting = (flags & LF_MODIFY_VIGNETTING) != 0,
            .vignetting_used_distance_fallback = vignetting_uses_distance_fallback
                && (flags & LF_MODIFY_VIGNETTING) != 0,
            .applied_scaling = applied_scaling,
        };
        if (
            !receipt.applied_distortion && !receipt.applied_tca
            && !receipt.applied_vignetting
        ) {
            return with_manual_optics(std::move(receipt), input, settings);
        }

        AlignedSamples<std::uint16_t> color_corrected(input.samples.size());
        std::copy(input.samples.begin(), input.samples.end(), color_corrected.data());
        if (receipt.applied_vignetting) {
            const auto modified = modifier.ApplyColorModification(
                color_corrected.data(),
                0.0F,
                0.0F,
                static_cast<int>(width),
                static_cast<int>(height),
                LF_CR_3(RED, GREEN, BLUE),
                static_cast<int>(input.row_stride_bytes)
            );
            if (!modified) {
                receipt.applied_vignetting = false;
                receipt.vignetting_used_distance_fallback = false;
            }
        }

        PixelBuffer output = input;
        if (receipt.applied_distortion || receipt.applied_tca) {
            const auto pixel_count = static_cast<std::size_t>(width) * height;
            const auto batch_rows = std::min(remap_rows_per_batch, height);
            std::vector<float> coordinates(
                static_cast<std::size_t>(width) * batch_rows * rgb_channels * 2U
            );
            for (std::uint32_t row = 0U; row < height; row += batch_rows) {
                const auto rows = std::min(batch_rows, height - row);
                const auto remapped = modifier.ApplySubpixelGeometryDistortion(
                    0.0F,
                    static_cast<float>(row),
                    static_cast<int>(width),
                    static_cast<int>(rows),
                    coordinates.data()
                );
                if (!remapped) {
                    throw DecodeError(
                        DecodeErrorCode::internal,
                        0,
                        "Lensfun unexpectedly declined an enabled optical remap"
                    );
                }
                for (std::uint32_t local_y = 0U; local_y < rows; ++local_y) {
                    const auto output_y = row + local_y;
                    for (std::uint32_t x = 0U; x < width; ++x) {
                        const auto pixel = static_cast<std::size_t>(local_y) * width + x;
                        for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                            const auto coordinate = (pixel * rgb_channels + channel) * 2U;
                            output.samples[(static_cast<std::size_t>(output_y) * width + x)
                                * rgb_channels + channel] = bilinear_sample_channel(
                                std::span<const std::uint16_t>(
                                    color_corrected.data(),
                                    pixel_count * rgb_channels
                                ),
                                input.dimensions,
                                coordinates[coordinate],
                                coordinates[coordinate + 1U],
                                channel
                            );
                        }
                    }
                }
            }
        } else if (receipt.applied_vignetting) {
            std::copy(
                color_corrected.data(),
                color_corrected.data() + static_cast<std::ptrdiff_t>(input.samples.size()),
                output.samples.begin()
            );
        }
        if (auto manual = apply_manual_optics(output, settings); manual.has_value()) {
            output = std::move(*manual);
        }
        return OpticsCorrectionResult{
            .receipt = std::move(receipt),
            .corrected_reference_rgb = std::move(output),
        };
    }

    [[nodiscard]] SceneLinearOpticsCorrectionResult correct_scene_linear_reference(
        const SceneLinearRgbFrame& input,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const override {
        validate_settings(settings);
        if (!settings.enabled) {
            return with_manual_optics(
                unavailable_receipt(
                    profile_catalog_.info(), OpticsProfileStatus::disabled
                ),
                input,
                settings
            );
        }
        if (!profile_catalog_.info().available) {
            return with_manual_optics(
                unavailable_receipt(
                    profile_catalog_.info(),
                    OpticsProfileStatus::provider_unavailable
                ),
                input,
                settings
            );
        }
        if (
            !finite_positive(metadata.focal_length_mm)
            || !profile_catalog_.profile_identity_available(metadata, settings)
        ) {
            return with_manual_optics(
                unavailable_receipt(
                    profile_catalog_.info(),
                    OpticsProfileStatus::insufficient_metadata
                ),
                input,
                settings
            );
        }
        validate_manual_scene_linear_input(input);

        const auto resolution = profile_catalog_.resolve_profile(metadata, settings);
        if (!resolution.match.has_value()) {
            return with_manual_optics(
                unavailable_receipt(profile_catalog_.info(), resolution.status),
                input,
                settings
            );
        }
        const auto& match = *resolution.match;

        const auto width = input.dimensions.width;
        const auto height = input.dimensions.height;
        if (
            width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            || height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        ) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "scene-linear optics image dimensions exceed Lensfun's integer API"
            );
        }

        const bool has_vignetting_aperture = finite_positive(metadata.aperture_f_number);
        const bool has_vignetting_distance = finite_positive(metadata.focus_distance_meters);
        const bool request_vignetting = settings.correct_vignetting && has_vignetting_aperture;
        const float vignetting_distance_meters = has_vignetting_distance
            ? static_cast<float>(metadata.focus_distance_meters)
            : 1000.0F;
        const bool vignetting_uses_distance_fallback = request_vignetting
            && !has_vignetting_distance;

#if LF_VERSION_MICRO >= 99
        lfModifier modifier(
            match.lens,
            static_cast<float>(metadata.focal_length_mm),
            crop_factor(metadata, *match.camera),
            static_cast<int>(width),
            static_cast<int>(height),
            LF_PF_F32
        );
        if (settings.correct_distortion) {
            modifier.EnableDistortionCorrection();
        }
        if (settings.correct_tca) {
            modifier.EnableTCACorrection();
        }
        if (request_vignetting) {
            modifier.EnableVignettingCorrection(
                static_cast<float>(metadata.aperture_f_number),
                vignetting_distance_meters
            );
        }

        auto flags = modifier.GetModFlags();
        const auto geometry_requested = (flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) != 0;
        bool applied_scaling = false;
        if (settings.automatic_scale && geometry_requested) {
            const auto automatic_scale = modifier.GetAutoScale(false);
            if (std::isfinite(automatic_scale) && automatic_scale > 0.0F) {
                modifier.EnableScaling(automatic_scale);
                flags = modifier.GetModFlags();
                applied_scaling = (flags & LF_MODIFY_SCALE) != 0;
            }
        }
#else
        int requested_flags = 0;
        if (settings.correct_distortion) {
            requested_flags |= LF_MODIFY_DISTORTION;
        }
        if (settings.correct_tca) {
            requested_flags |= LF_MODIFY_TCA;
        }
        if (request_vignetting) {
            requested_flags |= LF_MODIFY_VIGNETTING;
        }
        const auto geometry_requested =
            (requested_flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) != 0;
        if (settings.automatic_scale && geometry_requested) {
            requested_flags |= LF_MODIFY_SCALE;
        }

        lfModifier modifier(
            match.lens,
            crop_factor(metadata, *match.camera),
            static_cast<int>(width),
            static_cast<int>(height)
        );
        const auto flags = modifier.Initialize(
            match.lens,
            LF_PF_F32,
            static_cast<float>(metadata.focal_length_mm),
            request_vignetting ? static_cast<float>(metadata.aperture_f_number) : 0.0F,
            request_vignetting ? vignetting_distance_meters : 1000.0F,
            settings.automatic_scale && geometry_requested ? 0.0F : 1.0F,
            match.lens->Type,
            requested_flags,
            false
        );
        const bool applied_scaling = (flags & LF_MODIFY_SCALE) != 0;
#endif

        OpticsProfileReceipt receipt{
            .status = OpticsProfileStatus::matched,
            .provider_id = profile_catalog_.info().id,
            .provider_version = profile_catalog_.info().version,
            .camera_profile = match.camera_name,
            .lens_profile = match.lens_name,
            .distortion_available = (flags & LF_MODIFY_DISTORTION) != 0,
            .tca_available = (flags & LF_MODIFY_TCA) != 0,
            .vignetting_available = (flags & LF_MODIFY_VIGNETTING) != 0,
            .applied_distortion = (flags & LF_MODIFY_DISTORTION) != 0,
            .applied_tca = (flags & LF_MODIFY_TCA) != 0,
            .applied_vignetting = (flags & LF_MODIFY_VIGNETTING) != 0,
            .vignetting_used_distance_fallback = vignetting_uses_distance_fallback
                && (flags & LF_MODIFY_VIGNETTING) != 0,
            .applied_scaling = applied_scaling,
        };
        if (
            !receipt.applied_distortion && !receipt.applied_tca
            && !receipt.applied_vignetting
        ) {
            return with_manual_optics(std::move(receipt), input, settings);
        }

        AlignedSamples<float> color_corrected(input.samples.size());
        std::copy(input.samples.begin(), input.samples.end(), color_corrected.data());
        if (receipt.applied_vignetting) {
            const auto modified = modifier.ApplyColorModification(
                color_corrected.data(),
                0.0F,
                0.0F,
                static_cast<int>(width),
                static_cast<int>(height),
                LF_CR_3(RED, GREEN, BLUE),
                static_cast<int>(input.row_stride_bytes)
            );
            if (!modified) {
                receipt.applied_vignetting = false;
                receipt.vignetting_used_distance_fallback = false;
            }
        }

        SceneLinearRgbFrame output = input;
        if (receipt.applied_distortion || receipt.applied_tca) {
            const auto pixel_count = static_cast<std::size_t>(width) * height;
            const auto batch_rows = std::min(remap_rows_per_batch, height);
            std::vector<float> coordinates(
                static_cast<std::size_t>(width) * batch_rows * rgb_channels * 2U
            );
            for (std::uint32_t row = 0U; row < height; row += batch_rows) {
                const auto rows = std::min(batch_rows, height - row);
                const auto remapped = modifier.ApplySubpixelGeometryDistortion(
                    0.0F,
                    static_cast<float>(row),
                    static_cast<int>(width),
                    static_cast<int>(rows),
                    coordinates.data()
                );
                if (!remapped) {
                    throw DecodeError(
                        DecodeErrorCode::internal,
                        0,
                        "Lensfun unexpectedly declined an enabled scene-linear optical remap"
                    );
                }
                for (std::uint32_t local_y = 0U; local_y < rows; ++local_y) {
                    const auto output_y = row + local_y;
                    for (std::uint32_t x = 0U; x < width; ++x) {
                        const auto pixel = static_cast<std::size_t>(local_y) * width + x;
                        for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                            const auto coordinate = (pixel * rgb_channels + channel) * 2U;
                            output.samples[(static_cast<std::size_t>(output_y) * width + x)
                                * rgb_channels + channel] = bilinear_scene_linear_sample_channel(
                                std::span<const float>(
                                    color_corrected.data(),
                                    pixel_count * rgb_channels
                                ),
                                input.dimensions,
                                coordinates[coordinate],
                                coordinates[coordinate + 1U],
                                channel
                            );
                        }
                    }
                }
            }
        } else if (receipt.applied_vignetting) {
            std::copy(
                color_corrected.data(),
                color_corrected.data() + static_cast<std::ptrdiff_t>(input.samples.size()),
                output.samples.begin()
            );
        }
        if (auto manual = apply_manual_optics(output, settings); manual.has_value()) {
            output = std::move(*manual);
        }
        return SceneLinearOpticsCorrectionResult{
            .receipt = std::move(receipt),
            .corrected_scene_linear_rgb = std::move(output),
        };
    }

private:
    lensfun_profile_catalog::Catalog profile_catalog_;
};

#else

class LensfunOpticsProvider final : public OpticsProvider {
public:
    explicit LensfunOpticsProvider(const std::optional<std::filesystem::path>&) {
        info_.id = "lensfun";
        info_.version = "not-linked";
    }

    [[nodiscard]] const OpticsProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] OpticsCorrectionResult correct_reference_rgb(
        const PixelBuffer& input,
        const AssetMetadata&,
        const OpticsSettings& settings
    ) const override {
        validate_settings(settings);
        return with_manual_optics(
            unavailable_receipt(
                info_,
                settings.enabled ? OpticsProfileStatus::provider_unavailable
                                 : OpticsProfileStatus::disabled
            ),
            input,
            settings
        );
    }

    [[nodiscard]] SceneLinearOpticsCorrectionResult correct_scene_linear_reference(
        const SceneLinearRgbFrame& input,
        const AssetMetadata&,
        const OpticsSettings& settings
    ) const override {
        validate_settings(settings);
        return with_manual_optics(
            unavailable_receipt(
                info_,
                settings.enabled ? OpticsProfileStatus::provider_unavailable
                                 : OpticsProfileStatus::disabled
            ),
            input,
            settings
        );
    }

    [[nodiscard]] std::vector<OpticsProfileCandidate> profile_candidates(
        const AssetMetadata&
    ) const override {
        return {};
    }

private:
    OpticsProviderInfo info_;
};

#endif

} // namespace

OpticsSettings default_optics_settings() noexcept {
    return {};
}

std::string optics_settings_signature(const OpticsSettings& settings) {
    validate_settings(settings);
    std::ostringstream signature;
    signature << "shadow-optics-v" << optics_implementation_version
              << ";enabled=" << (settings.enabled ? 1 : 0)
              << ";distortion=" << (settings.correct_distortion ? 1 : 0)
              << ";tca=" << (settings.correct_tca ? 1 : 0)
              << ";vignetting=" << (settings.correct_vignetting ? 1 : 0)
              << ";auto-scale=" << (settings.automatic_scale ? 1 : 0)
              << ";manual-distortion=" << settings.manual_distortion
              << ";manual-tca-red-cyan=" << settings.manual_tca_red_cyan
              << ";manual-tca-blue-yellow=" << settings.manual_tca_blue_yellow
              << ";manual-vignetting=" << settings.manual_vignetting_amount
              << ";manual-vignetting-midpoint="
              << static_cast<unsigned int>(settings.manual_vignetting_midpoint);
    signature << ";camera-maker=" << settings.camera_profile_maker
              << ";camera-model=" << settings.camera_profile_model
              << ";lens-maker=" << settings.lens_profile_maker
              << ";lens-model=" << settings.lens_profile_model;
    return signature.str();
}

std::shared_ptr<const OpticsProvider> make_lensfun_optics_provider(
    std::optional<std::filesystem::path> database_directory
) {
    return std::make_shared<LensfunOpticsProvider>(std::move(database_directory));
}

} // namespace shadow::image
