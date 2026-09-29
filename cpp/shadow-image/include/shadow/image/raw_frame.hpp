#pragma once

#include <shadow/image/decoder_types.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace shadow::image {

// RawFrame is the provider-neutral boundary for *unprocessed* sensor samples. It owns a tight
// native-endian u16 plane in raw sensor coordinates: no crop, orientation, black subtraction,
// white balance, DNG opcode, demosaic, RGB conversion, highlight recovery or denoise has been
// applied. A provider may expose a non-Bayer RawFrame, but Bayer-only stages must require the
// explicit `bayer_2x2` layout rather than attempting to infer one from a display string.
//
// Shadow is still in its fast, pre-release iteration phase: this number names the one current
// RawFrame layout, not a backwards-compatibility promise. When the layout changes, all local
// providers are rebuilt together and obsolete artifacts are discarded.
// The per-site white levels name the calibrated coding white, while the
// optional per-site linear-response limits name the end of the sensor's
// calibrated linear region.  Bump this whenever either semantic changes so
// cached frames and their downstream GPU receipts cannot mix the two.
inline constexpr std::uint32_t raw_frame_schema_version = 2026082201U;

enum class RawFrameSampleEncoding : std::uint8_t {
    uint16_native,
};

enum class RawFrameCfaLayout : std::uint8_t {
    // A frame can preserve samples from a sensor whose mosaic Shadow does not yet understand.
    // Its metadata string remains useful for inspection, but demosaic must decline it.
    unknown,
    bayer_2x2,
    monochrome,
};

enum class RawCfaColor : std::uint8_t {
    unknown,
    red,
    green,
    blue,
};

// A provider resolves this for the individual source file from validated metadata or a locally
// installed calibration database. Shadow stores only a numeric model in a known unit, never an
// opaque vendor profile format. For `poisson_gaussian_per_cfa`, variance in raw-DN squared is
// `shot_noise_variance_per_dn * max(sample - black_level, 0) + read_noise_stddev_dn^2`.
inline constexpr std::uint32_t raw_sensor_noise_calibration_schema_version = 1U;

enum class RawSensorNoiseModel : std::uint8_t {
    unavailable,
    poisson_gaussian_per_cfa,
};

enum class RawSensorNoiseCalibrationSource : std::uint8_t {
    unavailable,
    embedded_metadata,
    // A configured provider matched a locally installed calibration profile for this exact
    // camera/ISO. The underlying profile stays private and is never put in a Shadow catalog,
    // recipe or public plugin.
    provider_calibration_profile,
};

struct RawSensorNoiseCalibration final {
    std::uint32_t schema_version = raw_sensor_noise_calibration_schema_version;
    RawSensorNoiseModel model = RawSensorNoiseModel::unavailable;
    RawSensorNoiseCalibrationSource source = RawSensorNoiseCalibrationSource::unavailable;
    // The ISO at which the model was resolved. It is zero only when no model is available.
    double iso_sensitivity = 0.0;
    // Order is the row-major CFA site order of `bayer_2x2`, matching the
    // black/white-level arrays. A later neural stage canonicalizes those sites
    // to R, G1, G2, B when it packs its model tensor.
    std::array<double, 4U> read_noise_stddev_dn{};
    std::array<double, 4U> shot_noise_variance_per_dn{};

    [[nodiscard]] bool valid() const noexcept {
        if (schema_version != raw_sensor_noise_calibration_schema_version) {
            return false;
        }
        if (model == RawSensorNoiseModel::unavailable) {
            return source == RawSensorNoiseCalibrationSource::unavailable && iso_sensitivity == 0.0;
        }
        if (model != RawSensorNoiseModel::poisson_gaussian_per_cfa
            || source == RawSensorNoiseCalibrationSource::unavailable
            || !std::isfinite(iso_sensitivity) || iso_sensitivity <= 0.0) {
            return false;
        }
        for (std::size_t index = 0U; index < read_noise_stddev_dn.size(); ++index) {
            if (!std::isfinite(read_noise_stddev_dn[index])
                || !std::isfinite(shot_noise_variance_per_dn[index])
                || read_noise_stddev_dn[index] < 0.0 || shot_noise_variance_per_dn[index] <= 0.0) {
                return false;
            }
        }
        return true;
    }
};

struct RawFrameDescriptor final {
    std::uint32_t schema_version = raw_frame_schema_version;
    // Cache-visible identity of the provider that extracted these sensor samples. Generic
    // fixtures may leave both fields empty, but a production provider must set both together.
    // This belongs to the frame rather than a later rendered receipt because two providers can
    // expose different unpacked samples or calibration for the same source bytes.
    std::string provider_id;
    std::string provider_version;
    // `storage_dimensions` covers the full sensor plane. `active_margins` and
    // `active_dimensions` identify the visible active rectangle inside it, before orientation.
    Dimensions storage_dimensions;
    Dimensions active_dimensions;
    Margins active_margins;
    std::int32_t orientation = 0;
    RawFrameSampleEncoding sample_encoding = RawFrameSampleEncoding::uint16_native;
    RawFrameCfaLayout cfa_layout = RawFrameCfaLayout::unknown;
    // Row-major colours at raw coordinates (0,0), (1,0), (0,1), (1,1). They are meaningful
    // only for `bayer_2x2`; green has two deliberately separate slots for per-site calibration.
    std::array<RawCfaColor, 4U> bayer_2x2{};
    std::string cfa_pattern;
    std::uint32_t bits_per_sample = 0;
    std::array<std::uint32_t, 4U> black_levels{};
    std::array<std::uint32_t, 4U> white_levels{};
    // Optional end of the calibrated linear-response range at each sensor
    // site, in the same unprocessed code-value domain as black/white.  It is
    // deliberately not a replacement for `white_levels`: samples between this
    // limit and coding white remain editable scene data, but their CFA chroma
    // ratio receives progressively less confidence near a highlight.
    std::array<std::uint32_t, 4U> linear_response_limits{};
    bool has_linear_response_limits = false;
    std::array<double, 4U> as_shot_neutral{};
    // Optional resolved source calibration for later RAW-domain denoise. A provider must leave
    // this unavailable rather than guessing by scanning an arbitrary profile binary.
    RawSensorNoiseCalibration sensor_noise;
    // Optional, row-major Camera RGB -> CIE XYZ matrix under a D50 white point. The input
    // order is the camera-linear RGB frame produced after the two green sites have been
    // reconstructed into its single green channel: `XYZ[j] = sum_i camera_rgb[i] * M[i][j]`.
    // A provider must leave `has_camera_to_xyz_d50` false rather than guessing the white point
    // or relabelling a camera-to-sRGB matrix as XYZ D50.
    std::array<double, 9U> camera_to_xyz_d50{};
    bool has_camera_to_xyz_d50 = false;
    // Optional, row-major CIE XYZ -> Camera RGB calibration under D65. This is deliberately the
    // inverse direction from `camera_to_xyz_d50`: LibRaw exposes `cam_xyz` in this form, before
    // the daylight channel gains folded into `rgb_cam`. It is therefore the calibration needed
    // to convert an authored illuminant white point into a physical camera neutral.
    std::array<double, 9U> xyz_to_camera_d65{};
    bool has_xyz_to_camera_d65 = false;
    // Optional, row-major Camera RGB -> linear sRGB/Rec.709 under D65. The camera input order is
    // canonical R, G, B after the two green CFA sites have been reconstructed into one channel:
    // `linear_srgb[row] = sum(camera_rgb[column] * M[row * 3 + column])`.
    //
    // LibRaw exposes this transform directly as `rgb_cam`. Keeping it distinct from the D50 XYZ
    // matrix prevents a provider from silently changing the transform's output space or white
    // point. A provider leaves the flag false when it cannot establish a usable transform.
    std::array<double, 9U> camera_to_linear_srgb_d65{};
    bool has_camera_to_linear_srgb_d65 = false;
    PendingCorrections declared_pending_corrections;
};

struct RawFrame final {
    RawFrameDescriptor descriptor;
    std::vector<std::uint16_t> samples;

    [[nodiscard]] bool valid() const noexcept {
        const auto width = static_cast<std::uint64_t>(descriptor.storage_dimensions.width);
        const auto height = static_cast<std::uint64_t>(descriptor.storage_dimensions.height);
        if (descriptor.schema_version != raw_frame_schema_version || width == 0U || height == 0U
            || descriptor.active_dimensions.width == 0U || descriptor.active_dimensions.height == 0U
            || descriptor.sample_encoding != RawFrameSampleEncoding::uint16_native
            || descriptor.cfa_pattern.empty() || !descriptor.sensor_noise.valid()
            || descriptor.provider_id.empty() != descriptor.provider_version.empty()) {
            return false;
        }
        const auto sample_count = width * height;
        if (sample_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
            || samples.size() != static_cast<std::size_t>(sample_count)) {
            return false;
        }
        const auto right = static_cast<std::uint64_t>(descriptor.active_margins.left)
                           + descriptor.active_dimensions.width + descriptor.active_margins.right;
        const auto bottom = static_cast<std::uint64_t>(descriptor.active_margins.top)
                            + descriptor.active_dimensions.height
                            + descriptor.active_margins.bottom;
        if (right != width || bottom != height || descriptor.bits_per_sample == 0U
            || descriptor.bits_per_sample > 16U) {
            return false;
        }
        for (std::size_t index = 0U; index < descriptor.black_levels.size(); ++index) {
            if (descriptor.black_levels[index] >= descriptor.white_levels[index]
                || !std::isfinite(descriptor.as_shot_neutral[index])
                || descriptor.as_shot_neutral[index] <= 0.0) {
                return false;
            }
            if (descriptor.has_linear_response_limits
                && (descriptor.linear_response_limits[index] <= descriptor.black_levels[index]
                    || descriptor.linear_response_limits[index]
                           > descriptor.white_levels[index])) {
                return false;
            }
        }
        const auto valid_declared_matrix = [](const auto& matrix, const bool declared) noexcept {
            if (!declared) {
                return true;
            }
            bool has_non_zero_coefficient = false;
            for (const auto value : matrix) {
                if (!std::isfinite(value)) {
                    return false;
                }
                has_non_zero_coefficient = has_non_zero_coefficient || value != 0.0;
            }
            return has_non_zero_coefficient;
        };
        if (!valid_declared_matrix(descriptor.camera_to_xyz_d50, descriptor.has_camera_to_xyz_d50)
            || !valid_declared_matrix(
                descriptor.xyz_to_camera_d65,
                descriptor.has_xyz_to_camera_d65
            )
            || !valid_declared_matrix(
                descriptor.camera_to_linear_srgb_d65,
                descriptor.has_camera_to_linear_srgb_d65
            )) {
            return false;
        }
        if (descriptor.cfa_layout != RawFrameCfaLayout::bayer_2x2) {
            return true;
        }
        std::size_t red = 0U;
        std::size_t green = 0U;
        std::size_t blue = 0U;
        for (const auto color : descriptor.bayer_2x2) {
            red += color == RawCfaColor::red ? 1U : 0U;
            green += color == RawCfaColor::green ? 1U : 0U;
            blue += color == RawCfaColor::blue ? 1U : 0U;
        }
        return red == 1U && green == 2U && blue == 1U;
    }

    [[nodiscard]] bool is_bayer_2x2() const noexcept {
        return valid() && descriptor.cfa_layout == RawFrameCfaLayout::bayer_2x2;
    }
};

} // namespace shadow::image
