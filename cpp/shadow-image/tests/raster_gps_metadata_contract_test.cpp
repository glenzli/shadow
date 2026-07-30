#include "contract_test_assertions.hpp"
#include "raster_exif.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;

void write_u16(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void write_u32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

void write_entry(
    std::vector<std::uint8_t>& bytes,
    const std::size_t offset,
    const std::uint16_t tag,
    const std::uint16_t type,
    const std::uint32_t count,
    const std::uint32_t value
) {
    write_u16(bytes, offset, tag);
    write_u16(bytes, offset + 2U, type);
    write_u32(bytes, offset + 4U, count);
    write_u32(bytes, offset + 8U, value);
}

void write_rational(
    std::vector<std::uint8_t>& bytes,
    const std::size_t offset,
    const std::uint32_t numerator,
    const std::uint32_t denominator
) {
    write_u32(bytes, offset, numerator);
    write_u32(bytes, offset + 4U, denominator);
}

void gps_ifd_projects_coordinates_and_signed_altitude() {
    std::vector<std::uint8_t> tiff(184U, 0U);
    tiff[0] = 'I';
    tiff[1] = 'I';
    write_u16(tiff, 2U, 42U);
    write_u32(tiff, 4U, 8U);

    write_u16(tiff, 8U, 1U);
    write_entry(tiff, 10U, 0x8825U, 4U, 1U, 32U);

    write_u16(tiff, 32U, 6U);
    write_entry(tiff, 34U, 0x0001U, 2U, 2U, static_cast<std::uint32_t>('N'));
    write_entry(tiff, 46U, 0x0002U, 5U, 3U, 128U);
    write_entry(tiff, 58U, 0x0003U, 2U, 2U, static_cast<std::uint32_t>('E'));
    write_entry(tiff, 70U, 0x0004U, 5U, 3U, 152U);
    write_entry(tiff, 82U, 0x0005U, 1U, 1U, 1U);
    write_entry(tiff, 94U, 0x0006U, 5U, 1U, 176U);

    write_rational(tiff, 128U, 31U, 1U);
    write_rational(tiff, 136U, 13U, 1U);
    write_rational(tiff, 144U, 48U, 1U);
    write_rational(tiff, 152U, 121U, 1U);
    write_rational(tiff, 160U, 28U, 1U);
    write_rational(tiff, 168U, 24U, 1U);
    write_rational(tiff, 176U, 50U, 1U);

    image::RasterExif exif;
    image::parse_tiff_exif(tiff, exif);
    expect(exif.has_gps_coordinates, "a valid GPS IFD exposes a coordinate pair");
    expect(
        std::abs(exif.gps_latitude_degrees - 31.23) < 1e-9,
        "latitude DMS converts to signed decimal degrees"
    );
    expect(
        std::abs(exif.gps_longitude_degrees - 121.47333333333333) < 1e-9,
        "longitude DMS converts to signed decimal degrees"
    );
    expect(
        exif.has_gps_altitude && exif.gps_altitude_meters == -50.0,
        "GPS altitude reference preserves below-sea-level sign"
    );
}

} // namespace

int main() {
    gps_ifd_projects_coordinates_and_signed_altitude();
    return shadow::image::test_support::failures == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
