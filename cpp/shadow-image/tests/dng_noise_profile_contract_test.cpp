#include "contract_test_assertions.hpp"
#include "dng_noise_profile.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;

enum class TestByteOrder : std::uint8_t {
    little,
    big,
};

struct Entry final {
    std::uint16_t tag = 0U;
    std::uint16_t type = 0U;
    std::vector<std::uint8_t> payload;
    std::uint32_t count = 0U;
};

void ensure_size(std::vector<std::uint8_t>& bytes, const std::size_t size) {
    if (bytes.size() < size) {
        bytes.resize(size, 0U);
    }
}

void write_u16(
    std::vector<std::uint8_t>& bytes,
    const std::size_t offset,
    const std::uint16_t value,
    const TestByteOrder order
) {
    ensure_size(bytes, offset + 2U);
    if (order == TestByteOrder::little) {
        bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
        bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    } else {
        bytes[offset] = static_cast<std::uint8_t>(value >> 8U);
        bytes[offset + 1U] = static_cast<std::uint8_t>(value & 0xffU);
    }
}

void write_u32(
    std::vector<std::uint8_t>& bytes,
    const std::size_t offset,
    const std::uint32_t value,
    const TestByteOrder order
) {
    ensure_size(bytes, offset + 4U);
    for (std::size_t index = 0U; index < 4U; ++index) {
        const std::size_t shift_index = order == TestByteOrder::little ? index : 3U - index;
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (shift_index * 8U)) & 0xffU);
    }
}

void write_u64(
    std::vector<std::uint8_t>& bytes,
    const std::size_t offset,
    const std::uint64_t value,
    const TestByteOrder order
) {
    ensure_size(bytes, offset + 8U);
    for (std::size_t index = 0U; index < 8U; ++index) {
        const std::size_t shift_index = order == TestByteOrder::little ? index : 7U - index;
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (shift_index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::vector<std::uint8_t>
u16_payload(const std::uint16_t value, const TestByteOrder order) {
    std::vector<std::uint8_t> payload;
    write_u16(payload, 0U, value, order);
    return payload;
}

[[nodiscard]] std::vector<std::uint8_t>
u32_payload(const std::uint32_t value, const TestByteOrder order) {
    std::vector<std::uint8_t> payload;
    write_u32(payload, 0U, value, order);
    return payload;
}

[[nodiscard]] std::vector<std::uint8_t>
double_payload(const std::span<const double> values, const TestByteOrder order) {
    std::vector<std::uint8_t> payload;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        write_u64(payload, index * 8U, std::bit_cast<std::uint64_t>(values[index]), order);
    }
    return payload;
}

[[nodiscard]] std::size_t write_ifd(
    std::vector<std::uint8_t>& bytes,
    const std::size_t offset,
    const std::span<const Entry> entries,
    const std::uint32_t next_ifd,
    const TestByteOrder order
) {
    write_u16(bytes, offset, static_cast<std::uint16_t>(entries.size()), order);
    const std::size_t structure_end = offset + 2U + entries.size() * 12U + 4U;
    ensure_size(bytes, structure_end);
    std::size_t payload_offset = structure_end;
    for (std::size_t index = 0U; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const std::size_t entry_offset = offset + 2U + index * 12U;
        write_u16(bytes, entry_offset, entry.tag, order);
        write_u16(bytes, entry_offset + 2U, entry.type, order);
        write_u32(bytes, entry_offset + 4U, entry.count, order);
        if (entry.payload.size() <= 4U) {
            ensure_size(bytes, entry_offset + 12U);
            for (std::size_t byte = 0U; byte < entry.payload.size(); ++byte) {
                bytes[entry_offset + 8U + byte] = entry.payload[byte];
            }
        } else {
            write_u32(bytes, entry_offset + 8U, static_cast<std::uint32_t>(payload_offset), order);
            ensure_size(bytes, payload_offset + entry.payload.size());
            for (std::size_t byte = 0U; byte < entry.payload.size(); ++byte) {
                bytes[payload_offset + byte] = entry.payload[byte];
            }
            payload_offset += entry.payload.size();
        }
    }
    write_u32(bytes, offset + 2U + entries.size() * 12U, next_ifd, order);
    return payload_offset;
}

[[nodiscard]] Entry
long_entry(const std::uint16_t tag, const std::uint32_t value, const TestByteOrder order) {
    return Entry{tag, 4U, u32_payload(value, order), 1U};
}

[[nodiscard]] Entry
short_entry(const std::uint16_t tag, const std::uint16_t value, const TestByteOrder order) {
    return Entry{tag, 3U, u16_payload(value, order), 1U};
}

[[nodiscard]] Entry dng_version_entry() {
    return Entry{50'706U, 1U, {1U, 4U, 0U, 0U}, 4U};
}

[[nodiscard]] std::vector<std::uint8_t> raw_dng(
    const TestByteOrder order,
    const std::span<const double> noise,
    const std::span<const std::uint8_t> colors = {}
) {
    std::vector<std::uint8_t> bytes(8U, 0U);
    bytes[0] = order == TestByteOrder::little ? 'I' : 'M';
    bytes[1] = bytes[0];
    write_u16(bytes, 2U, 42U, order);
    write_u32(bytes, 4U, 8U, order);
    std::vector<Entry> entries{
        long_entry(254U, 0U, order),
        short_entry(262U, 32'803U, order),
        dng_version_entry(),
    };
    if (!colors.empty()) {
        entries.push_back(
            Entry{
                50'710U,
                1U,
                std::vector<std::uint8_t>(colors.begin(), colors.end()),
                static_cast<std::uint32_t>(colors.size()),
            }
        );
    }
    if (!noise.empty()) {
        entries.push_back(
            Entry{
                51'041U,
                12U,
                double_payload(noise, order),
                static_cast<std::uint32_t>(noise.size()),
            }
        );
    }
    static_cast<void>(write_ifd(bytes, 8U, entries, 0U, order));
    return bytes;
}

[[nodiscard]] bool near(const double left, const double right) noexcept {
    return std::abs(left - right) <= 1e-12;
}

void shared_profile_supports_both_byte_orders() {
    constexpr std::array values{2.62079e-05, 3.49646e-07};
    for (const auto order : {TestByteOrder::little, TestByteOrder::big}) {
        const auto bytes = raw_dng(order, values);
        const auto receipt = image::parse_dng_noise_profile(bytes);
        expect(receipt.exact(), "a valid shared DNG NoiseProfile is exact");
        expect(receipt.ifds_visited == 1U, "a root RAW IFD is visited exactly once");
        for (std::size_t color = 0U; color < 3U; ++color) {
            expect(
                near(receipt.profile.normalized_scale[color], values[0]),
                "shared scale expands to every RGB plane"
            );
            expect(
                near(receipt.profile.normalized_offset[color], values[1]),
                "shared offset expands to every RGB plane"
            );
        }
    }
}

void plane_order_maps_to_canonical_rgb() {
    constexpr std::array colors{std::uint8_t{2U}, std::uint8_t{0U}, std::uint8_t{1U}};
    constexpr std::array values{
        0.30,
        0.003,
        0.10,
        0.001,
        0.20,
        0.002,
    };
    const auto bytes = raw_dng(TestByteOrder::little, values, colors);
    const auto receipt = image::parse_dng_noise_profile(bytes);
    expect(receipt.exact(), "a three-plane profile with RGB colors is exact");
    expect(
        near(receipt.profile.normalized_scale[0], 0.10)
            && near(receipt.profile.normalized_offset[0], 0.001),
        "red follows CFAPlaneColor rather than tag position"
    );
    expect(
        near(receipt.profile.normalized_scale[1], 0.20)
            && near(receipt.profile.normalized_offset[1], 0.002),
        "green follows CFAPlaneColor rather than tag position"
    );
    expect(
        near(receipt.profile.normalized_scale[2], 0.30)
            && near(receipt.profile.normalized_offset[2], 0.003),
        "blue follows CFAPlaneColor rather than tag position"
    );

    constexpr std::array default_order_values{
        0.10,
        0.001,
        0.20,
        0.002,
        0.30,
        0.003,
    };
    const auto default_order =
        image::parse_dng_noise_profile(raw_dng(TestByteOrder::little, default_order_values));
    expect(
        default_order.exact() && near(default_order.profile.normalized_scale[0], 0.10)
            && near(default_order.profile.normalized_scale[1], 0.20)
            && near(default_order.profile.normalized_scale[2], 0.30),
        "an omitted CFAPlaneColor uses the DNG RGB default"
    );
}

void raw_sub_ifd_is_selected_over_preview() {
    constexpr std::array values{0.25, 0.0025};
    const auto order = TestByteOrder::little;
    std::vector<std::uint8_t> bytes(8U, 0U);
    bytes[0] = 'I';
    bytes[1] = 'I';
    write_u16(bytes, 2U, 42U, order);
    write_u32(bytes, 4U, 8U, order);
    constexpr std::uint32_t raw_ifd_offset = 64U;
    const std::array root_entries{
        long_entry(254U, 1U, order),
        short_entry(262U, 2U, order),
        Entry{330U, 13U, u32_payload(raw_ifd_offset, order), 1U},
        dng_version_entry(),
    };
    static_cast<void>(write_ifd(bytes, 8U, root_entries, 0U, order));
    const std::array raw_entries{
        long_entry(254U, 0U, order),
        short_entry(262U, 32'803U, order),
        Entry{51'041U, 12U, double_payload(values, order), 2U},
    };
    static_cast<void>(write_ifd(bytes, raw_ifd_offset, raw_entries, 0U, order));

    const auto receipt = image::parse_dng_noise_profile(bytes);
    expect(receipt.exact(), "NoiseProfile is read from the primary CFA SubIFD");
    expect(receipt.ifds_visited == 2U, "the root and raw SubIFD are both bounded");
}

void absence_and_unsupported_mappings_fail_closed() {
    const auto absent = raw_dng(TestByteOrder::little, {});
    expect(
        image::parse_dng_noise_profile(absent).status == image::DngNoiseProfileStatus::absent,
        "a DNG without NoiseProfile remains explicitly uncalibrated"
    );

    constexpr std::array values{0.1, 0.001};
    // Rebuild with a LinearizationTable marker before NoiseProfile so the
    // parser cannot silently apply normalized coefficients to uncertain DN.
    std::vector<std::uint8_t> guarded(8U, 0U);
    guarded[0] = 'I';
    guarded[1] = 'I';
    write_u16(guarded, 2U, 42U, TestByteOrder::little);
    write_u32(guarded, 4U, 8U, TestByteOrder::little);
    const std::array entries{
        long_entry(254U, 0U, TestByteOrder::little),
        short_entry(262U, 32'803U, TestByteOrder::little),
        dng_version_entry(),
        short_entry(50'712U, 0U, TestByteOrder::little),
        Entry{
            51'041U,
            12U,
            double_payload(values, TestByteOrder::little),
            2U,
        },
    };
    static_cast<void>(write_ifd(guarded, 8U, entries, 0U, TestByteOrder::little));
    expect(
        image::parse_dng_noise_profile(guarded).status
            == image::DngNoiseProfileStatus::unsupported_profile_shape,
        "a DNG linearization table prevents an unproven DN conversion"
    );
}

void malformed_and_ambiguous_profiles_are_rejected() {
    constexpr std::array invalid_values{0.0, -0.1};
    expect(
        image::parse_dng_noise_profile(raw_dng(TestByteOrder::little, invalid_values)).status
            == image::DngNoiseProfileStatus::malformed,
        "non-positive scale and negative offset are malformed"
    );

    constexpr std::array unsupported_values{0.1, 0.001, 0.2, 0.002};
    expect(
        image::parse_dng_noise_profile(raw_dng(TestByteOrder::little, unsupported_values)).status
            == image::DngNoiseProfileStatus::unsupported_profile_shape,
        "non-shared, non-RGB profile shapes are not guessed"
    );

    constexpr std::array valid_values{0.1, 0.001};
    auto cycle = raw_dng(TestByteOrder::little, valid_values);
    const std::size_t entry_count = 4U;
    write_u32(cycle, 8U + 2U + entry_count * 12U, 8U, TestByteOrder::little);
    expect(
        image::parse_dng_noise_profile(cycle).status == image::DngNoiseProfileStatus::malformed,
        "an IFD cycle is rejected instead of traversed"
    );

    auto truncated = raw_dng(TestByteOrder::little, valid_values);
    truncated.resize(8U + 2U + entry_count * 12U + 4U);
    expect(
        image::parse_dng_noise_profile(truncated).status == image::DngNoiseProfileStatus::malformed,
        "an out-of-bounds NoiseProfile payload is rejected"
    );

    auto overlapping = raw_dng(TestByteOrder::little, valid_values);
    const std::size_t noise_entry_offset = 8U + 2U + 3U * 12U;
    write_u32(overlapping, noise_entry_offset + 8U, 8U, TestByteOrder::little);
    expect(
        image::parse_dng_noise_profile(overlapping).status
            == image::DngNoiseProfileStatus::malformed,
        "a NoiseProfile payload cannot overlap its IFD structure"
    );

    auto pre_profile_version = raw_dng(TestByteOrder::little, valid_values);
    const std::size_t version_entry_offset = 8U + 2U + 2U * 12U;
    pre_profile_version[version_entry_offset + 8U + 1U] = 2U;
    expect(
        image::parse_dng_noise_profile(pre_profile_version).status
            == image::DngNoiseProfileStatus::malformed,
        "NoiseProfile is not accepted under a pre-1.3 DNG declaration"
    );

    auto ambiguous = raw_dng(TestByteOrder::little, valid_values);
    constexpr std::uint32_t second_ifd_offset = 128U;
    write_u32(ambiguous, 8U + 2U + entry_count * 12U, second_ifd_offset, TestByteOrder::little);
    ensure_size(ambiguous, second_ifd_offset);
    const std::array second_raw_entries{
        long_entry(254U, 0U, TestByteOrder::little),
        short_entry(262U, 32'803U, TestByteOrder::little),
    };
    static_cast<void>(
        write_ifd(ambiguous, second_ifd_offset, second_raw_entries, 0U, TestByteOrder::little)
    );
    expect(
        image::parse_dng_noise_profile(ambiguous).status
            == image::DngNoiseProfileStatus::ambiguous_raw_ifd,
        "multiple primary CFA Raw IFDs are never guessed"
    );
}

void normalized_model_resolves_to_site_ordered_dn_units() {
    image::DngNoiseProfileReceipt receipt{
        .status = image::DngNoiseProfileStatus::exact,
        .profile =
            image::DngNoiseProfile{
                .normalized_scale = {0.10, 0.20, 0.30},
                .normalized_offset = {0.0001, 0.0004, 0.0009},
            },
        .ifds_visited = 1U,
    };
    image::RawFrameDescriptor descriptor;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        image::RawCfaColor::green,
        image::RawCfaColor::red,
        image::RawCfaColor::blue,
        image::RawCfaColor::green,
    };
    descriptor.black_levels = {100U, 200U, 300U, 400U};
    descriptor.white_levels = {1'100U, 2'200U, 3'300U, 4'400U};

    const auto calibration = image::resolve_dng_noise_profile(receipt, descriptor, 800.0);
    expect(
        calibration.model == image::RawSensorNoiseModel::poisson_gaussian_per_cfa
            && calibration.source == image::RawSensorNoiseCalibrationSource::embedded_metadata,
        "an exact profile becomes embedded per-CFA calibration"
    );
    expect(
        near(calibration.shot_noise_variance_per_dn[0], 200.0)
            && near(calibration.read_noise_stddev_dn[0], 20.0),
        "the first green site uses green coefficients and its own DN range"
    );
    expect(
        near(calibration.shot_noise_variance_per_dn[1], 200.0)
            && near(calibration.read_noise_stddev_dn[1], 20.0),
        "the red site uses red coefficients and its own DN range"
    );
    expect(
        near(calibration.shot_noise_variance_per_dn[2], 900.0)
            && near(calibration.read_noise_stddev_dn[2], 90.0),
        "the blue site uses blue coefficients and its own DN range"
    );
    expect(
        near(calibration.shot_noise_variance_per_dn[3], 800.0)
            && near(calibration.read_noise_stddev_dn[3], 80.0),
        "the second green site remains distinct in site order"
    );
    expect(
        calibration.iso_sensitivity == 800.0 && calibration.valid(),
        "the source ISO and complete DN model form a valid calibration"
    );
}

} // namespace

int main() {
    shared_profile_supports_both_byte_orders();
    plane_order_maps_to_canonical_rgb();
    raw_sub_ifd_is_selected_over_preview();
    absence_and_unsupported_mappings_fail_closed();
    malformed_and_ambiguous_profiles_are_rejected();
    normalized_model_resolves_to_site_ordered_dn_units();
    return shadow::image::test_support::failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
