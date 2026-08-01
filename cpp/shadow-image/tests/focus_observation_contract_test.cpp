#include "contract_test_assertions.hpp"

#include <shadow/image/decoder.hpp>
#include <shadow/image/focus_observation.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;

[[nodiscard]] bool near(const double actual, const double expected) {
    return std::abs(actual - expected) < 1.0e-9;
}

void write_little_u16(
    std::array<std::uint8_t, 76U>& record,
    const std::size_t offset,
    const std::uint16_t value
) {
    record[offset] = static_cast<std::uint8_t>(value & 0xffU);
    record[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void nikon_expeed_7_area_is_bounded_and_oriented() {
    std::array<std::uint8_t, 76U> record{};
    write_little_u16(record, 60U, 8'256U);
    write_little_u16(record, 62U, 5'504U);
    write_little_u16(record, 64U, 4'128U);
    write_little_u16(record, 66U, 1'376U);
    write_little_u16(record, 68U, 412U);
    write_little_u16(record, 70U, 550U);
    record[72U] = 1U;

    const auto landscape = image::nikon_focus_observation(
        400U,
        image::FocusRecordByteOrder::little_endian,
        record,
        0
    );
    expect(landscape.has_value(), "valid Nikon AFInfo2 V0400 yields an observation");
    expect(
        landscape.has_value() && near(landscape->center_x, 0.5)
            && near(landscape->center_y, 0.25)
            && near(landscape->width, 412.0 / 8'256.0)
            && near(landscape->height, 550.0 / 5'504.0),
        "Nikon area coordinates are normalized against their declared AF canvas"
    );
    expect(
        landscape.has_value() && landscape->focus_confirmed
            && near(landscape->confidence, 1.0)
            && landscape->source == image::FocusObservationSource::camera_focus_area,
        "Nikon focus result and area provenance remain explicit"
    );

    const auto clockwise = image::nikon_focus_observation(
        400U,
        image::FocusRecordByteOrder::little_endian,
        record,
        6
    );
    expect(
        clockwise.has_value() && near(clockwise->center_x, 0.75)
            && near(clockwise->center_y, 0.5)
            && near(clockwise->width, 550.0 / 5'504.0)
            && near(clockwise->height, 412.0 / 8'256.0),
        "LibRaw clockwise orientation maps the area into the displayed source"
    );
}

void malformed_or_unsupported_nikon_records_fail_closed() {
    std::array<std::uint8_t, 76U> record{};
    write_little_u16(record, 60U, 100U);
    write_little_u16(record, 62U, 100U);
    write_little_u16(record, 64U, 101U);
    write_little_u16(record, 66U, 50U);

    expect(
        !image::nikon_focus_observation(
             400U,
             image::FocusRecordByteOrder::little_endian,
             record,
             0
         ).has_value(),
        "out-of-bounds Nikon coordinates never become a guessed focus point"
    );
    expect(
        !image::nikon_focus_observation(
             300U,
             image::FocusRecordByteOrder::little_endian,
             record,
             0
         ).has_value(),
        "unknown Nikon AFInfo2 layouts remain unsupported"
    );
}

void sony_location_is_a_point_with_distinct_provenance() {
    const auto observation = image::sony_focus_observation(
        std::array<std::uint16_t, 4U>{8'640U, 5'760U, 2'160U, 4'320U},
        5
    );
    expect(observation.has_value(), "valid Sony FocusLocation yields an observation");
    expect(
        observation.has_value() && near(observation->center_x, 0.75)
            && near(observation->center_y, 0.75)
            && near(observation->width, 0.0) && near(observation->height, 0.0),
        "Sony point coordinates use the same oriented source space without inventing an area"
    );
    expect(
        observation.has_value() && !observation->focus_confirmed
            && near(observation->confidence, 0.85)
            && observation->source
                == image::FocusObservationSource::camera_focus_location,
        "Sony location provenance does not claim a focus-confirmation result"
    );
}

void real_camera_focus_when_configured() {
    const char* fixture = std::getenv("SHADOW_TEST_FOCUS_RAW");
    if (fixture == nullptr || *fixture == '\0') {
        return;
    }
    const auto provider = image::make_libraw_decoder_provider();
    const auto decoder = provider->open(fixture);
    const auto& observation = decoder->metadata().focus_observation;
    expect(
        observation.has_value(),
        "configured real RAW source exposes a camera focus observation"
    );
    expect(
        observation.has_value() && observation->schema_version
                == image::focus_observation_schema_version
            && observation->center_x >= 0.0 && observation->center_x <= 1.0
            && observation->center_y >= 0.0 && observation->center_y <= 1.0
            && observation->confidence > 0.0 && observation->confidence <= 1.0,
        "real camera focus observation remains bounded and versioned"
    );
}

} // namespace

int main() {
    nikon_expeed_7_area_is_bounded_and_oriented();
    malformed_or_unsupported_nikon_records_fail_closed();
    sony_location_is_a_point_with_distinct_provenance();
    real_camera_focus_when_configured();
    return shadow::image::test_support::failures == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
