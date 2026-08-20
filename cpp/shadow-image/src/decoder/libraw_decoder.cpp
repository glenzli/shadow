#include <shadow/image/decoder_error.hpp>
#include <shadow/image/decoder_metadata.hpp>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/decoder_types.hpp>
#include <shadow/image/focus_observation.hpp>
#include <shadow/image/libraw_development_settings.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_development_receipt.hpp>
#include <shadow/image/raw_frame.hpp>
#include <shadow/image/reference_pixels.hpp>

#include "dng_noise_profile.hpp"
#include "libraw_reference_development.hpp"
#include "libraw_runtime.hpp"

#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace shadow::image {

namespace {

using ProcessedImage =
    std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)>;
// This version covers the display-orientation semantics of cached embedded-preview descriptors.
// It is deliberately separate from the raw-frame and rendered-RGB contracts: the JPEG bytes do
// not change, but their catalog geometry must match the auto-oriented image that Qt presents.
inline constexpr std::uint32_t libraw_embedded_preview_geometry_contract_version = 1U;
// LibRaw parses Nikon's MakerNote compression tag even when its public decoder cannot safely
// develop that bitstream.  Values 13 and 14 are Nikon High Efficiency and High Efficiency*,
// respectively (see LibRaw's libraw_makernotes_t documentation).  Treating either as a normal
// unpackable Bayer source is unsafe: current public LibRaw builds can advertise a decoder and
// then fail, or crash, only after the host has entered the RAW-development path.
inline constexpr std::uint16_t nikon_nef_high_efficiency_compression = 13U;
inline constexpr std::uint16_t nikon_nef_high_efficiency_star_compression = 14U;

[[nodiscard]] bool
libraw_nef_compression_requires_external_provider(const LibRaw& decoder) noexcept {
    const std::uint16_t compression = decoder.imgdata.makernotes.nikon.NEFCompression;
    return compression == nikon_nef_high_efficiency_compression
           || compression == nikon_nef_high_efficiency_star_compression;
}

[[nodiscard]] std::string dng_version_string(const unsigned version) {
    if (version == 0U) {
        return {};
    }

    std::ostringstream output;
    output << ((version >> 24U) & 0xffU) << '.' << ((version >> 16U) & 0xffU) << '.'
           << ((version >> 8U) & 0xffU) << '.' << (version & 0xffU);
    return output.str();
}

[[nodiscard]] std::string cfa_pattern(LibRaw& decoder) {
    if (decoder.imgdata.idata.filters == 0U) {
        return "none/linear";
    }

    std::string result;
    result.reserve(4);
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const int color_index = decoder.COLOR(row, column);
            const bool valid_index = color_index >= 0 && color_index < 4;
            result.push_back(valid_index ? decoder.imgdata.idata.cdesc[color_index] : '?');
        }
    }
    return result;
}

[[nodiscard]] RawCfaColor raw_cfa_color(LibRaw& decoder, const int row, const int column) noexcept {
    const int color_index = decoder.COLOR(row, column);
    if (color_index < 0 || color_index >= 4) {
        return RawCfaColor::unknown;
    }
    switch (decoder.imgdata.idata.cdesc[color_index]) {
    case 'R':
    case 'r':
        return RawCfaColor::red;
    case 'G':
    case 'g':
        return RawCfaColor::green;
    case 'B':
    case 'b':
        return RawCfaColor::blue;
    default:
        return RawCfaColor::unknown;
    }
}

[[nodiscard]] std::array<RawCfaColor, 4U> raw_frame_bayer_2x2(LibRaw& decoder) noexcept {
    return {
        raw_cfa_color(decoder, 0, 0),
        raw_cfa_color(decoder, 0, 1),
        raw_cfa_color(decoder, 1, 0),
        raw_cfa_color(decoder, 1, 1),
    };
}

[[nodiscard]] RawFrameCfaLayout
raw_frame_cfa_layout(LibRaw& decoder, const std::array<RawCfaColor, 4U>& bayer_2x2) noexcept {
    if (decoder.imgdata.idata.filters == 0U) {
        return RawFrameCfaLayout::monochrome;
    }
    // LibRaw uses this marker for X-Trans. Its first 2x2 cells must not be mistaken for a
    // Bayer repeat, even if their colours happen to contain one R, two G and one B.
    if (decoder.imgdata.idata.filters == 9U) {
        return RawFrameCfaLayout::unknown;
    }
    std::size_t red = 0U;
    std::size_t green = 0U;
    std::size_t blue = 0U;
    for (const auto color : bayer_2x2) {
        red += color == RawCfaColor::red ? 1U : 0U;
        green += color == RawCfaColor::green ? 1U : 0U;
        blue += color == RawCfaColor::blue ? 1U : 0U;
    }
    return red == 1U && green == 2U && blue == 1U ? RawFrameCfaLayout::bayer_2x2
                                                  : RawFrameCfaLayout::unknown;
}

[[nodiscard]] std::array<int, 4U>
raw_frame_color_indices(LibRaw& decoder, const RawFrameCfaLayout layout) noexcept {
    if (layout == RawFrameCfaLayout::monochrome) {
        return {0, 0, 0, 0};
    }
    return {
        decoder.COLOR(0, 0),
        decoder.COLOR(0, 1),
        decoder.COLOR(1, 0),
        decoder.COLOR(1, 1),
    };
}

[[nodiscard]] bool valid_color_index(const int index) noexcept {
    return index >= 0 && index < 4;
}

[[nodiscard]] constexpr std::uint32_t
combined_black_level(const std::uint32_t common, const std::uint32_t correction) noexcept {
    return correction > std::numeric_limits<std::uint32_t>::max() - common
               ? std::numeric_limits<std::uint32_t>::max()
               : common + correction;
}

static_assert(combined_black_level(255U, 1U) == 256U);
static_assert(
    combined_black_level(std::numeric_limits<std::uint32_t>::max(), 1U)
    == std::numeric_limits<std::uint32_t>::max()
);

[[nodiscard]] std::uint32_t
raw_frame_black_level(const libraw_colordata_t& color, const int color_index) noexcept {
    const auto index = static_cast<std::size_t>(color_index);
    // LibRaw defines `black` as the common sensor floor and `cblack[0..3]` as
    // per-channel corrections to that floor. They are additive, not competing
    // alternatives. Treating a small correction (for example Canon's 1 DN)
    // as the complete black level lifts that CFA channel by hundreds of DN
    // and turns clipped/high-key regions pink after white balance.
    return combined_black_level(color.black, color.cblack[index]);
}

[[nodiscard]] std::uint32_t
raw_frame_white_level(const libraw_colordata_t& color) noexcept {
    // `linear_max` is LibRaw's per-channel boundary for the recorded linear-response region,
    // not the CFA sample coding white.  Using it here silently makes the Bayer normalisation
    // and sensor-clipping stages discard the remaining sensor headroom one channel at a time.
    // LibRaw's `maximum` is the calibrated code-value white reference for this RAW frame; keep
    // it shared across CFA sites and leave channel relationships available to later highlight
    // treatment rather than clipping them during source preparation.
    return color.maximum;
}

[[nodiscard]] bool positive_finite(const double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] double camera_white_balance_multiplier(
    const libraw_data_t& data,
    const int requested_color_index
) noexcept {
    const auto requested = static_cast<std::size_t>(requested_color_index);
    if (positive_finite(data.color.cam_mul[requested])) {
        return data.color.cam_mul[requested];
    }

    // Some three-colour RAWs describe their CFA as RGBG but carry only one green multiplier.
    // Resolve the second green from that equivalent colour component rather than treating the
    // absent fourth coefficient as a neutral value.
    const char requested_color = data.idata.cdesc[requested];
    for (std::size_t candidate = 0U; candidate < 4U; ++candidate) {
        if (data.idata.cdesc[candidate] == requested_color
            && positive_finite(data.color.cam_mul[candidate])) {
            return data.color.cam_mul[candidate];
        }
    }
    return 0.0;
}

[[nodiscard]] std::array<double, 4U> raw_frame_as_shot_neutral(
    const libraw_data_t& data,
    const RawFrameCfaLayout layout,
    const std::array<RawCfaColor, 4U>& bayer_2x2,
    const std::array<int, 4U>& color_indices
) {
    if (layout == RawFrameCfaLayout::monochrome) {
        return {1.0, 1.0, 1.0, 1.0};
    }

    std::array<double, 4U> neutral{};
    bool embedded_is_valid = true;
    for (std::size_t site = 0U; site < neutral.size(); ++site) {
        const auto color_index = static_cast<std::size_t>(color_indices[site]);
        neutral[site] = data.color.dng_levels.asshotneutral[color_index];
        embedded_is_valid = embedded_is_valid && positive_finite(neutral[site]);
    }
    if (embedded_is_valid) {
        return neutral;
    }

    // DNG AsShotNeutral is the inverse of the camera-space WB multipliers up to a common scale.
    // Normalize that scale to the mean of the two green CFA sites, matching the conventional
    // neutral representation consumed by Shadow's future RAW white-balance stage.
    for (std::size_t site = 0U; site < neutral.size(); ++site) {
        const double multiplier = camera_white_balance_multiplier(data, color_indices[site]);
        if (!positive_finite(multiplier)) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "LibRaw did not expose a usable AsShotNeutral or camera white balance"
            );
        }
        neutral[site] = 1.0 / multiplier;
    }

    double green_neutral_sum = 0.0;
    std::size_t green_site_count = 0U;
    for (std::size_t site = 0U; site < neutral.size(); ++site) {
        if (bayer_2x2[site] == RawCfaColor::green) {
            green_neutral_sum += neutral[site];
            ++green_site_count;
        }
    }
    const double normalization = green_site_count == 0U
                                     ? neutral.front()
                                     : green_neutral_sum / static_cast<double>(green_site_count);
    if (!positive_finite(normalization)) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            LIBRAW_NOT_IMPLEMENTED,
            "LibRaw camera white balance cannot be normalized"
        );
    }
    for (auto& value : neutral) {
        value /= normalization;
    }
    return neutral;
}

[[nodiscard]] std::optional<std::array<double, 9U>> camera_to_linear_srgb_d65(
    const libraw_data_t& data,
    const RawFrameCfaLayout layout,
    const std::array<int, 4U>& color_indices
) noexcept {
    if (layout != RawFrameCfaLayout::bayer_2x2) {
        return std::nullopt;
    }

    std::array<bool, 4U> used_color_indices{};
    for (const int color_index : color_indices) {
        if (!valid_color_index(color_index)) {
            return std::nullopt;
        }
        used_color_indices[static_cast<std::size_t>(color_index)] = true;
    }

    std::array<double, 9U> matrix{};
    std::array<bool, 3U> has_canonical_input{};
    for (std::size_t source = 0U; source < used_color_indices.size(); ++source) {
        if (!used_color_indices[source]) {
            continue;
        }
        std::size_t canonical_input = 0U;
        switch (data.idata.cdesc[source]) {
        case 'R':
        case 'r':
            canonical_input = 0U;
            break;
        case 'G':
        case 'g':
            canonical_input = 1U;
            break;
        case 'B':
        case 'b':
            canonical_input = 2U;
            break;
        default:
            return std::nullopt;
        }
        has_canonical_input[canonical_input] = true;
        for (std::size_t output = 0U; output < 3U; ++output) {
            const double coefficient = data.color.rgb_cam[output][source];
            if (!std::isfinite(coefficient)) {
                return std::nullopt;
            }
            // If LibRaw keeps two distinct green inputs, Shadow's demosaiced camera RGB has one
            // green channel, so both green coefficients contribute to that canonical column.
            matrix[output * 3U + canonical_input] += coefficient;
        }
    }

    bool has_non_zero_coefficient = false;
    for (const double value : matrix) {
        has_non_zero_coefficient = has_non_zero_coefficient || value != 0.0;
    }
    if (!std::ranges::all_of(has_canonical_input, [](const bool present) { return present; })
        || !has_non_zero_coefficient) {
        return std::nullopt;
    }
    return matrix;
}

[[nodiscard]] std::optional<std::array<double, 9U>> xyz_to_camera_d65(
    const libraw_data_t& data,
    const RawFrameCfaLayout layout,
    const std::array<int, 4U>& color_indices
) noexcept {
    if (layout != RawFrameCfaLayout::bayer_2x2) {
        return std::nullopt;
    }

    std::array<double, 9U> matrix{};
    std::array<std::uint32_t, 3U> canonical_counts{};
    std::array<bool, 4U> used_color_indices{};
    for (const int color_index : color_indices) {
        if (!valid_color_index(color_index)) {
            return std::nullopt;
        }
        used_color_indices[static_cast<std::size_t>(color_index)] = true;
    }
    for (std::size_t source = 0U; source < used_color_indices.size(); ++source) {
        if (!used_color_indices[source]) {
            continue;
        }
        std::size_t canonical_output = 0U;
        switch (data.idata.cdesc[source]) {
        case 'R':
        case 'r':
            canonical_output = 0U;
            break;
        case 'G':
        case 'g':
            canonical_output = 1U;
            break;
        case 'B':
        case 'b':
            canonical_output = 2U;
            break;
        default:
            return std::nullopt;
        }
        std::array<double, 3U> source_row{};
        bool source_non_zero = false;
        for (std::size_t xyz = 0U; xyz < 3U; ++xyz) {
            const double coefficient = data.color.cam_xyz[source][xyz];
            if (!std::isfinite(coefficient)) {
                return std::nullopt;
            }
            source_row[xyz] = coefficient;
            source_non_zero = source_non_zero || coefficient != 0.0;
        }
        // LibRaw may name a fourth CFA colour `G` while leaving its calibration row empty.
        // It represents a second green site, not another independent colour transform.
        if (!source_non_zero) {
            continue;
        }
        ++canonical_counts[canonical_output];
        for (std::size_t xyz = 0U; xyz < 3U; ++xyz) {
            matrix[canonical_output * 3U + xyz] += source_row[xyz];
        }
    }
    for (std::size_t output = 0U; output < canonical_counts.size(); ++output) {
        if (canonical_counts[output] == 0U) {
            return std::nullopt;
        }
        for (std::size_t xyz = 0U; xyz < 3U; ++xyz) {
            matrix[output * 3U + xyz] /= static_cast<double>(canonical_counts[output]);
        }
    }
    const bool non_zero =
        std::ranges::any_of(matrix, [](const double coefficient) { return coefficient != 0.0; });
    return non_zero ? std::optional{matrix} : std::nullopt;
}

[[nodiscard]] PreviewFormat
preview_format(const LibRaw_internal_thumbnail_formats format) noexcept {
    switch (format) {
    case LIBRAW_INTERNAL_THUMBNAIL_JPEG:
        return PreviewFormat::jpeg;
    case LIBRAW_INTERNAL_THUMBNAIL_JPEGXL:
        return PreviewFormat::jpeg_xl;
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_THUMB:
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_YCBCR:
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_RGB:
    case LIBRAW_INTERNAL_THUMBNAIL_PPM:
    case LIBRAW_INTERNAL_THUMBNAIL_PPM16:
    case LIBRAW_INTERNAL_THUMBNAIL_DNG_YCBCR:
        return PreviewFormat::bitmap;
    default:
        return PreviewFormat::unknown;
    }
}

[[nodiscard]] PreviewFormat preview_format(const LibRaw_image_formats format) noexcept {
    switch (format) {
    case LIBRAW_IMAGE_JPEG:
        return PreviewFormat::jpeg;
    case LIBRAW_IMAGE_BITMAP:
        return PreviewFormat::bitmap;
    case LIBRAW_IMAGE_JPEGXL:
        return PreviewFormat::jpeg_xl;
    case LIBRAW_IMAGE_H265:
        return PreviewFormat::h265;
    default:
        return PreviewFormat::unknown;
    }
}

[[nodiscard]] Margins image_margins(const libraw_image_sizes_t& sizes) noexcept {
    const auto used_width = static_cast<std::uint32_t>(sizes.left_margin) + sizes.width;
    const auto used_height = static_cast<std::uint32_t>(sizes.top_margin) + sizes.height;
    const auto right = sizes.raw_width > used_width ? sizes.raw_width - used_width : 0U;
    const auto bottom = sizes.raw_height > used_height ? sizes.raw_height - used_height : 0U;
    return Margins{sizes.left_margin, sizes.top_margin, right, bottom};
}

[[nodiscard]] std::optional<double> libraw_gps_coordinate(
    const float (&parts)[3],
    const char reference,
    const double maximum
) noexcept {
    const double degrees = parts[0];
    const double minutes = parts[1];
    const double seconds = parts[2];
    if (!std::isfinite(degrees) || !std::isfinite(minutes) || !std::isfinite(seconds)
        || degrees < 0.0 || degrees > maximum || minutes < 0.0 || minutes >= 60.0 || seconds < 0.0
        || seconds >= 60.0) {
        return std::nullopt;
    }
    double coordinate = degrees + minutes / 60.0 + seconds / 3'600.0;
    if (reference == 'S' || reference == 's' || reference == 'W' || reference == 'w') {
        coordinate = -coordinate;
    } else if (reference != 'N' && reference != 'n' && reference != 'E' && reference != 'e') {
        return std::nullopt;
    }
    return coordinate;
}

[[nodiscard]] AssetMetadata read_metadata(LibRaw& decoder) {
    const auto& identity = decoder.imgdata.idata;
    const auto& sizes = decoder.imgdata.sizes;
    const auto& color = decoder.imgdata.color;
    const auto& dng = color.dng_levels;
    const auto& capture = decoder.imgdata.other;
    const auto& lens = decoder.imgdata.lens;

    AssetMetadata metadata;
    metadata.make = identity.make;
    metadata.model = identity.model;
    metadata.normalized_make = identity.normalized_make;
    metadata.normalized_model = identity.normalized_model;
    metadata.dng_version = dng_version_string(identity.dng_version);
    metadata.raw_count = identity.raw_count;
    metadata.raw_dimensions = Dimensions{sizes.raw_width, sizes.raw_height};
    metadata.image_dimensions = Dimensions{sizes.width, sizes.height};
    metadata.margins = image_margins(sizes);
    metadata.orientation = sizes.flip;
    metadata.cfa_pattern = cfa_pattern(decoder);
    metadata.sensor_colors = static_cast<std::uint32_t>(identity.colors);
    metadata.sensor_bits = color.raw_bps;
    metadata.black_level = color.black;
    metadata.white_level = color.maximum;
    for (std::size_t index = 0; index < metadata.as_shot_neutral.size(); ++index) {
        metadata.as_shot_neutral[index] = dng.asshotneutral[index];
    }
    metadata.baseline_exposure = dng.baseline_exposure;
    metadata.iso_speed = capture.iso_speed;
    metadata.exposure_time_seconds = capture.shutter;
    metadata.aperture_f_number = capture.aperture;
    metadata.focal_length_mm = capture.focal_len;
    const auto& maker_notes = decoder.imgdata.makernotes;
    const int focus_record_count =
        std::clamp(maker_notes.common.afcount, 0, LIBRAW_AFDATA_MAXCOUNT);
    for (int index = 0; index < focus_record_count; ++index) {
        const auto& record = maker_notes.common.afdata[static_cast<std::size_t>(index)];
        if (record.AFInfoData_tag != 0x00b7U || record.AFInfoData == nullptr) {
            continue;
        }
        if (record.AFInfoData_order != 0x4949 && record.AFInfoData_order != 0x4d4d) {
            continue;
        }
        const auto byte_order = record.AFInfoData_order == 0x4949
                                    ? FocusRecordByteOrder::little_endian
                                    : FocusRecordByteOrder::big_endian;
        metadata.focus_observation = nikon_focus_observation(
            record.AFInfoData_version,
            byte_order,
            std::span<const std::uint8_t>{
                record.AFInfoData,
                static_cast<std::size_t>(record.AFInfoData_length),
            },
            sizes.flip
        );
        if (metadata.focus_observation.has_value()) {
            break;
        }
    }
    if (!metadata.focus_observation.has_value()) {
        const auto& location = maker_notes.sony.FocusLocation;
        metadata.focus_observation = sony_focus_observation(
            std::array<std::uint16_t, 4U>{
                location[0U],
                location[1U],
                location[2U],
                location[3U],
            },
            sizes.flip
        );
    }
    metadata.captured_at_unix_seconds = static_cast<std::int64_t>(capture.timestamp);
    if (capture.parsed_gps.gpsparsed != 0) {
        const auto latitude =
            libraw_gps_coordinate(capture.parsed_gps.latitude, capture.parsed_gps.latref, 90.0);
        const auto longitude =
            libraw_gps_coordinate(capture.parsed_gps.longitude, capture.parsed_gps.longref, 180.0);
        if (latitude.has_value() && longitude.has_value()) {
            metadata.has_gps_coordinates = true;
            metadata.gps_latitude_degrees = *latitude;
            metadata.gps_longitude_degrees = *longitude;
        }
        if (std::isfinite(capture.parsed_gps.altitude)) {
            metadata.has_gps_altitude = true;
            metadata.gps_altitude_meters = capture.parsed_gps.altref == 1
                                               ? -static_cast<double>(capture.parsed_gps.altitude)
                                               : static_cast<double>(capture.parsed_gps.altitude);
        }
    }
    metadata.lens_make = lens.LensMake;
    metadata.lens_model = lens.Lens;
    metadata.focal_length_35mm = lens.FocalLengthIn35mmFormat;
    return metadata;
}

// LibRaw's `sizes.flip` describes the output orientation rather than standard EXIF orientation
// values. Its current 5 and 6 cases transpose the displayed axes. Keep cached preview geometry
// in that display coordinate system because the Qt thumbnail reader applies the same orientation
// while loading the encoded preview bytes. Without this, an upright portrait preview receives a
// landscape justified tile and exposes empty side bars around the image.
[[nodiscard]] Dimensions display_oriented_preview_dimensions(
    Dimensions dimensions,
    const std::int32_t orientation
) noexcept {
    if (orientation == 5 || orientation == 6) {
        std::swap(dimensions.width, dimensions.height);
    }
    return dimensions;
}

[[nodiscard]] std::vector<PreviewDescriptor> read_previews(const libraw_data_t& data) {
    std::vector<PreviewDescriptor> previews;
    const int count = std::max(0, data.thumbs_list.thumbcount);
    previews.reserve(static_cast<std::size_t>(count));

    for (int index = 0; index < count; ++index) {
        const auto& candidate = data.thumbs_list.thumblist[index];
        const bool placeholder =
            candidate.tlength == 0U && candidate.twidth == 0U && candidate.theight == 0U;
        if (placeholder) {
            continue;
        }

        PreviewDescriptor descriptor;
        descriptor.id = static_cast<std::size_t>(index);
        descriptor.format = preview_format(candidate.tformat);
        descriptor.dimensions = display_oriented_preview_dimensions(
            Dimensions{candidate.twidth, candidate.theight},
            data.sizes.flip
        );
        descriptor.bits_per_channel = static_cast<std::uint16_t>(candidate.tmisc & 31U);
        descriptor.channels = static_cast<std::uint16_t>(candidate.tmisc >> 5U);
        descriptor.encoded_bytes = candidate.tlength;
        descriptor.decodable = descriptor.format != PreviewFormat::unknown;
        previews.push_back(std::move(descriptor));
    }
    return previews;
}

class LibRawSession final : public DecodeSession {
  public:
    explicit LibRawSession(
        std::filesystem::path path,
        LibRawDevelopmentSettings settings,
        ProviderInfo provider_info
    ) : path_(std::move(path)), settings_(settings), provider_info_(std::move(provider_info)) {
        decoder_.imgdata.rawparams.max_raw_memory_mb = 2'048U;
        require_libraw_success(libraw_open_path(decoder_, path_), "open_file");

        metadata_ = read_metadata(decoder_);
        dng_noise_profile_ = read_dng_noise_profile(path_);
        previews_ = read_previews(decoder_.imgdata);
        libraw_decoder_info_t decoder_info{};
        const bool decoder_advertises_unpack =
            decoder_.get_decoder_info(&decoder_info) == LIBRAW_SUCCESS
            && (decoder_info.decoder_flags
                & (LIBRAW_DECODER_UNSUPPORTED_FORMAT | LIBRAW_DECODER_NOTSET))
                   == 0U;
        // Preserve factual metadata and any camera JPEG for browse mode, but do not let public
        // LibRaw enter its unsafe HE/HE* development path. The photo router still gives an
        // independently installed private provider the opportunity to claim this source after
        // seeing these public capabilities.
        const bool decoder_can_unpack =
            decoder_advertises_unpack
            && !libraw_nef_compression_requires_external_provider(decoder_);
        capabilities_.metadata = true;
        capabilities_.embedded_previews = !previews_.empty();
        capabilities_.raw_frame =
            decoder_can_unpack
            && (decoder_.imgdata.idata.filters != 0U || decoder_.imgdata.idata.colors == 1);
        capabilities_.reference_rgb = decoder_can_unpack;
        capabilities_.pending_corrections = libraw_pending_corrections(decoder_.imgdata);
        reference_developer_ = std::make_unique<LibRawReferenceDeveloper>(
            path_,
            settings_,
            provider_info_,
            metadata_.image_dimensions,
            capabilities_.reference_rgb,
            capabilities_.raw_frame,
            decoder_.imgdata.makernotes.nikon.NEFCompression
        );
        capabilities_.raw_development = reference_developer_->capabilities();
    }

    [[nodiscard]] const AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const PreviewDescriptor> previews() const noexcept override {
        return previews_;
    }

    [[nodiscard]] const RawDevelopmentCapabilities&
    raw_development_capabilities() const noexcept override {
        return capabilities_.raw_development;
    }

    [[nodiscard]] RawDevelopmentPlanNegotiation
    negotiate_raw_development_plan(const RawDevelopmentPlan& plan) const noexcept override {
        return reference_developer_->negotiate(plan);
    }

    [[nodiscard]] PreviewPayload decode_preview(const std::size_t id) override {
        const auto descriptor = std::find_if(
            previews_.begin(),
            previews_.end(),
            [id](const PreviewDescriptor& candidate) { return candidate.id == id; }
        );
        if (descriptor == previews_.end() || !descriptor->decodable || id > INT_MAX) {
            throw DecodeError(
                DecodeErrorCode::no_preview,
                LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL,
                "requested preview is not available"
            );
        }

        require_libraw_success(decoder_.unpack_thumb_ex(static_cast<int>(id)), "unpack_thumb_ex");
        int result = LIBRAW_SUCCESS;
        ProcessedImage image(decoder_.dcraw_make_mem_thumb(&result), &LibRaw::dcraw_clear_mem);
        if (!image) {
            throw_libraw_error(result, "dcraw_make_mem_thumb");
        }

        PreviewPayload payload;
        payload.descriptor = *descriptor;
        payload.descriptor.format = preview_format(image->type);
        if (image->width != 0U && image->height != 0U) {
            payload.descriptor.dimensions = display_oriented_preview_dimensions(
                Dimensions{image->width, image->height},
                metadata_.orientation
            );
        }
        if (image->bits != 0U) {
            payload.descriptor.bits_per_channel = image->bits;
        }
        if (image->colors != 0U) {
            payload.descriptor.channels = image->colors;
        }
        payload.descriptor.encoded_bytes = image->data_size;
        payload.descriptor.decodable = payload.descriptor.format != PreviewFormat::unknown;
        payload.byte_order =
            payload.descriptor.format == PreviewFormat::bitmap && image->bits == 16U
                ? ByteOrder::native
                : ByteOrder::not_applicable;
        payload.bytes.assign(image->data, image->data + image->data_size);
        return payload;
    }

    [[nodiscard]] RawFrame decode_raw_frame() override {
        require_raw_frame();
        ensure_unpacked();
        const auto& sizes = decoder_.imgdata.sizes;
        const auto* raw_image = decoder_.imgdata.rawdata.raw_image;
        if (raw_image == nullptr) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "decoder did not return a single-plane integer RAW frame"
            );
        }

        const std::size_t width = sizes.raw_width;
        const std::size_t height = sizes.raw_height;
        if (height != 0U && width > std::numeric_limits<std::size_t>::max() / height) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                LIBRAW_TOO_BIG,
                "RAW frame dimensions overflow the address space"
            );
        }

        const std::size_t source_stride =
            sizes.raw_pitch == 0U ? width : sizes.raw_pitch / sizeof(std::uint16_t);
        if (source_stride < width || sizes.raw_pitch % sizeof(std::uint16_t) != 0U) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_DATA_ERROR,
                "RAW frame row stride is invalid"
            );
        }

        RawFrame frame;
        auto& descriptor = frame.descriptor;
        descriptor.schema_version = raw_frame_schema_version;
        descriptor.provider_id = provider_info_.id;
        descriptor.provider_version = provider_info_.version;
        descriptor.storage_dimensions = metadata_.raw_dimensions;
        descriptor.active_dimensions = metadata_.image_dimensions;
        descriptor.active_margins = metadata_.margins;
        descriptor.orientation = metadata_.orientation;
        descriptor.sample_encoding = RawFrameSampleEncoding::uint16_native;
        descriptor.bayer_2x2 = raw_frame_bayer_2x2(decoder_);
        descriptor.cfa_layout = raw_frame_cfa_layout(decoder_, descriptor.bayer_2x2);
        descriptor.cfa_pattern = metadata_.cfa_pattern;
        descriptor.bits_per_sample = metadata_.sensor_bits;
        const auto color_indices = raw_frame_color_indices(decoder_, descriptor.cfa_layout);
        if (std::ranges::any_of(color_indices, [](const int color_index) {
                return !valid_color_index(color_index);
            })) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "LibRaw returned an invalid CFA colour index"
            );
        }
        const auto& color = decoder_.imgdata.color;
        for (std::size_t site = 0U; site < descriptor.black_levels.size(); ++site) {
            descriptor.black_levels[site] = raw_frame_black_level(color, color_indices[site]);
            descriptor.white_levels[site] = raw_frame_white_level(color);
        }
        descriptor.sensor_noise =
            resolve_dng_noise_profile(dng_noise_profile_, descriptor, metadata_.iso_speed);
        descriptor.as_shot_neutral = raw_frame_as_shot_neutral(
            decoder_.imgdata,
            descriptor.cfa_layout,
            descriptor.bayer_2x2,
            color_indices
        );
        if (const auto matrix =
                camera_to_linear_srgb_d65(decoder_.imgdata, descriptor.cfa_layout, color_indices)) {
            descriptor.camera_to_linear_srgb_d65 = *matrix;
            descriptor.has_camera_to_linear_srgb_d65 = true;
        }
        if (const auto matrix =
                xyz_to_camera_d65(decoder_.imgdata, descriptor.cfa_layout, color_indices)) {
            descriptor.xyz_to_camera_d65 = *matrix;
            descriptor.has_xyz_to_camera_d65 = true;
        }
        descriptor.declared_pending_corrections = capabilities_.pending_corrections;
        frame.samples.resize(width * height);

        for (std::size_t row = 0; row < height; ++row) {
            const auto* source = raw_image + (row * source_stride);
            auto* destination = frame.samples.data() + (row * width);
            std::copy_n(source, width, destination);
        }
        if (!frame.valid()) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "decoder returned an invalid provider-neutral RAW frame"
            );
        }
        return frame;
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        return reference_developer_->render(default_raw_development_plan());
    }

    [[nodiscard]] PixelBuffer render_reference_rgb(const RawDevelopmentPlan& plan) const override {
        return reference_developer_->render(plan);
    }

    [[nodiscard]] PixelBuffer
    render_reference_rgb_for_preview(const std::uint32_t max_edge) const override {
        return reference_developer_->render_preview(max_edge, preview_raw_development_plan());
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge,
        const RawDevelopmentPlan& plan
    ) const override {
        return reference_developer_->render_preview(max_edge, plan);
    }

  private:
    void ensure_unpacked() {
        require_raw_frame();
        if (unpacked_) {
            return;
        }
        require_libraw_success(decoder_.unpack(), "unpack");
        unpacked_ = true;
    }

    void require_raw_frame() const {
        if (capabilities_.raw_frame)
            return;
        if (capabilities_.reference_rgb) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                LIBRAW_NOT_IMPLEMENTED,
                "this LibRaw source uses the processed-RGB compatibility path instead of Shadow "
                "RawFrame"
            );
        }
        const std::uint16_t compression = decoder_.imgdata.makernotes.nikon.NEFCompression;
        if (compression == nikon_nef_high_efficiency_compression) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                LIBRAW_NOT_IMPLEMENTED,
                "Nikon NEF High Efficiency compression requires an external decoder provider"
            );
        }
        if (compression == nikon_nef_high_efficiency_star_compression) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                LIBRAW_NOT_IMPLEMENTED,
                "Nikon NEF High Efficiency* compression requires an external decoder provider"
            );
        }
        throw DecodeError(
            DecodeErrorCode::unsupported,
            LIBRAW_NOT_IMPLEMENTED,
            "LibRaw cannot expose a provider-neutral RAW frame for this source"
        );
    }

    std::filesystem::path path_;
    LibRawDevelopmentSettings settings_;
    ProviderInfo provider_info_;
    LibRaw decoder_;
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;
    DngNoiseProfileReceipt dng_noise_profile_;
    std::unique_ptr<LibRawReferenceDeveloper> reference_developer_;
    std::vector<PreviewDescriptor> previews_;
    bool unpacked_ = false;
};

class LibRawProvider final : public DecoderProvider {
  public:
    explicit LibRawProvider(LibRawDevelopmentSettings settings) : settings_(settings) {
        validate_libraw_development_settings(settings_);
        const unsigned capabilities = LibRaw::capabilities();
        info_.id = "libraw";
        // Provider version participates in generated-proxy/cache identity. Include Shadow's
        // reference/output contracts so a transfer or gamut-mapping change cannot reuse bytes
        // generated under the same linked LibRaw release.
        info_.version = "lr=" + std::string(LibRaw::version()) + ";cap="
                        + std::to_string(libraw_reference_development_contract_version) + ";linear="
                        + std::to_string(processed_linear_reference_rgb_contract_version)
                        + ";receipt=" + std::to_string(raw_development_receipt_schema_version)
                        + ";plan=" + std::to_string(raw_development_plan_schema_version)
                        + ";frame=" + std::to_string(raw_frame_schema_version) + "-n"
                        + std::to_string(dng_noise_profile_contract_version) + ";preview="
                        + std::to_string(libraw_embedded_preview_geometry_contract_version)
                        + ";display=" + std::to_string(display_srgb8_output_transform_version)
                        + ";s=" + compact_libraw_development_settings_identity(settings_);
        if (info_.version.size() > 128U) {
            throw std::invalid_argument("LibRaw provider cache identity exceeds 128 bytes");
        }
        info_.dng_sdk = (capabilities & LIBRAW_CAPS_DNGSDK) != 0U;
        info_.rawspeed = (capabilities & (LIBRAW_CAPS_RAWSPEED | LIBRAW_CAPS_RAWSPEED3)) != 0U;
        info_.jpeg = (capabilities & LIBRAW_CAPS_JPEG) != 0U;
    }

    [[nodiscard]] const ProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] std::unique_ptr<DecodeSession>
    open(const std::filesystem::path& path) const override {
        return std::make_unique<LibRawSession>(path, settings_, info_);
    }

  private:
    LibRawDevelopmentSettings settings_;
    ProviderInfo info_;
};

} // namespace

std::unique_ptr<DecoderProvider>
make_libraw_decoder_provider(const LibRawDevelopmentSettings settings) {
    return std::make_unique<LibRawProvider>(settings);
}

} // namespace shadow::image
