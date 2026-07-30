#pragma once

#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_development_plan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image {

struct DecodeCapabilities final {
    bool metadata = false;
    bool embedded_previews = false;
    // An independently owned, sensor-coordinate RawFrame is available. This is deliberately
    // distinct from `reference_rgb`: a provider must never claim the latter's already-developed
    // pixels are suitable input for Bayer-domain work.
    bool raw_frame = false;
    bool reference_rgb = false;
    PendingCorrections pending_corrections;
    RawDevelopmentCapabilities raw_development;
};

struct AssetMetadata final {
    std::string make;
    std::string model;
    std::string normalized_make;
    std::string normalized_model;
    std::string dng_version;
    std::uint32_t raw_count = 0;
    Dimensions raw_dimensions;
    Dimensions image_dimensions;
    Margins margins;
    std::int32_t orientation = 0;
    std::string cfa_pattern;
    std::uint32_t sensor_colors = 0;
    std::uint32_t sensor_bits = 0;
    std::uint32_t black_level = 0;
    std::uint32_t white_level = 0;
    std::array<double, 4> as_shot_neutral{};
    double baseline_exposure = 0.0;
    double iso_speed = 0.0;
    double exposure_time_seconds = 0.0;
    double aperture_f_number = 0.0;
    double focal_length_mm = 0.0;
    // Approximate focus distance in metres when a provider can establish it. Zero means
    // unknown, never infinity or a guessed substitute. Lens vignetting calibration is
    // distance-dependent, so optical correction must leave that component disabled without it.
    double focus_distance_meters = 0.0;
    std::int64_t captured_at_unix_seconds = 0;
    bool has_gps_coordinates = false;
    double gps_latitude_degrees = 0.0;
    double gps_longitude_degrees = 0.0;
    bool has_gps_altitude = false;
    double gps_altitude_meters = 0.0;
    std::string lens_make;
    std::string lens_model;
    double focal_length_35mm = 0.0;
};

struct PreviewDescriptor final {
    std::size_t id = 0;
    PreviewFormat format = PreviewFormat::unknown;
    Dimensions dimensions;
    std::uint16_t bits_per_channel = 0;
    std::uint16_t channels = 0;
    std::uint64_t encoded_bytes = 0;
    bool decodable = false;
};

struct PreviewPayload final {
    PreviewDescriptor descriptor;
    ByteOrder byte_order = ByteOrder::not_applicable;
    std::vector<std::uint8_t> bytes;
};

[[nodiscard]] std::optional<std::size_t> select_best_preview(
    std::span<const PreviewDescriptor> previews
) noexcept;

[[nodiscard]] std::string_view to_string(PreviewFormat format) noexcept;

} // namespace shadow::image
