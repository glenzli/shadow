#include <shadow/image/camera_profile.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expect_close(
    const double actual,
    const double expected,
    const double tolerance,
    const std::string_view message
) {
    expect(std::abs(actual - expected) <= tolerance, message);
}

enum class TestTiffType : std::uint16_t {
    byte = 1U,
    ascii = 2U,
    short_value = 3U,
    long_value = 4U,
    rational = 5U,
    undefined = 7U,
    signed_rational = 10U,
    float_value = 11U,
};

struct TestTag final {
    std::uint16_t tag = 0U;
    TestTiffType type = TestTiffType::undefined;
    std::uint32_t count = 0U;
    std::vector<std::byte> payload;
};

class DcpFixtureBuilder final {
public:
    explicit DcpFixtureBuilder(const image::DcpByteOrder byte_order)
        : byte_order_(byte_order) {
    }

    void add_raw(
        const std::uint16_t tag,
        const TestTiffType type,
        const std::uint32_t count,
        std::vector<std::byte> payload
    ) {
        tags_.push_back(TestTag{
            .tag = tag,
            .type = type,
            .count = count,
            .payload = std::move(payload),
        });
    }

    void add_string(
        const std::uint16_t tag,
        const TestTiffType type,
        const std::string_view value
    ) {
        std::vector<std::byte> bytes;
        bytes.reserve(value.size() + 1U);
        for (const char character : value) {
            bytes.push_back(static_cast<std::byte>(character));
        }
        bytes.push_back(std::byte{0});
        add_raw(tag, type, static_cast<std::uint32_t>(bytes.size()), std::move(bytes));
    }

    void add_short(const std::uint16_t tag, const std::uint16_t value) {
        std::vector<std::byte> bytes;
        append_u16(bytes, value);
        add_raw(tag, TestTiffType::short_value, 1U, std::move(bytes));
    }

    void add_long(const std::uint16_t tag, const std::uint32_t value) {
        std::vector<std::byte> bytes;
        append_u32(bytes, value);
        add_raw(tag, TestTiffType::long_value, 1U, std::move(bytes));
    }

    void add_dimensions(
        const std::uint16_t tag,
        const std::uint32_t hue,
        const std::uint32_t saturation,
        const std::uint32_t value
    ) {
        std::vector<std::byte> bytes;
        append_u32(bytes, hue);
        append_u32(bytes, saturation);
        append_u32(bytes, value);
        add_raw(tag, TestTiffType::long_value, 3U, std::move(bytes));
    }

    void add_matrix(
        const std::uint16_t tag,
        const std::vector<std::pair<std::int32_t, std::int32_t>>& values
    ) {
        std::vector<std::byte> bytes;
        for (const auto [numerator, denominator] : values) {
            append_i32(bytes, numerator);
            append_i32(bytes, denominator);
        }
        add_raw(
            tag,
            TestTiffType::signed_rational,
            static_cast<std::uint32_t>(values.size()),
            std::move(bytes)
        );
    }

    void add_signed_rational(
        const std::uint16_t tag,
        const std::int32_t numerator,
        const std::int32_t denominator
    ) {
        std::vector<std::byte> bytes;
        append_i32(bytes, numerator);
        append_i32(bytes, denominator);
        add_raw(tag, TestTiffType::signed_rational, 1U, std::move(bytes));
    }

    void add_floats(const std::uint16_t tag, const std::vector<float>& values) {
        std::vector<std::byte> bytes;
        for (const float value : values) {
            append_u32(bytes, std::bit_cast<std::uint32_t>(value));
        }
        add_raw(
            tag,
            TestTiffType::float_value,
            static_cast<std::uint32_t>(values.size()),
            std::move(bytes)
        );
    }

    [[nodiscard]] std::vector<std::byte> finish() {
        std::stable_sort(tags_.begin(), tags_.end(), [](const TestTag& left, const TestTag& right) {
            return left.tag < right.tag;
        });
        const std::size_t ifd_bytes = 2U + tags_.size() * 12U + 4U;
        std::vector<std::byte> result(8U + ifd_bytes, std::byte{0});
        result[0] = static_cast<std::byte>(
            byte_order_ == image::DcpByteOrder::little_endian ? 'I' : 'M'
        );
        result[1] = result[0];
        write_u16(result, 2U, 0x4352U);
        write_u32(result, 4U, 8U);
        write_u16(result, 8U, static_cast<std::uint16_t>(tags_.size()));

        for (std::size_t index = 0U; index < tags_.size(); ++index) {
            const auto& tag = tags_[index];
            const std::size_t entry_offset = 10U + index * 12U;
            write_u16(result, entry_offset, tag.tag);
            write_u16(
                result,
                entry_offset + 2U,
                static_cast<std::uint16_t>(tag.type)
            );
            write_u32(result, entry_offset + 4U, tag.count);
            if (tag.payload.size() <= 4U) {
                std::copy(
                    tag.payload.begin(),
                    tag.payload.end(),
                    result.begin() + static_cast<std::ptrdiff_t>(entry_offset + 8U)
                );
            } else {
                while (result.size() % 4U != 0U) {
                    result.push_back(std::byte{0});
                }
                write_u32(
                    result,
                    entry_offset + 8U,
                    static_cast<std::uint32_t>(result.size())
                );
                result.insert(result.end(), tag.payload.begin(), tag.payload.end());
            }
        }
        return result;
    }

private:
    void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) const {
        const std::size_t offset = bytes.size();
        bytes.resize(offset + 2U);
        write_u16(bytes, offset, value);
    }

    void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) const {
        const std::size_t offset = bytes.size();
        bytes.resize(offset + 4U);
        write_u32(bytes, offset, value);
    }

    void append_i32(std::vector<std::byte>& bytes, const std::int32_t value) const {
        append_u32(bytes, std::bit_cast<std::uint32_t>(value));
    }

    void write_u16(
        std::vector<std::byte>& bytes,
        const std::size_t offset,
        const std::uint16_t value
    ) const {
        if (byte_order_ == image::DcpByteOrder::little_endian) {
            bytes[offset] = static_cast<std::byte>(value & 0xFFU);
            bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
        } else {
            bytes[offset] = static_cast<std::byte>((value >> 8U) & 0xFFU);
            bytes[offset + 1U] = static_cast<std::byte>(value & 0xFFU);
        }
    }

    void write_u32(
        std::vector<std::byte>& bytes,
        const std::size_t offset,
        const std::uint32_t value
    ) const {
        for (std::size_t index = 0U; index < 4U; ++index) {
            const std::size_t target = byte_order_ == image::DcpByteOrder::little_endian
                ? index
                : 3U - index;
            bytes[offset + target] =
                static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
        }
    }

    image::DcpByteOrder byte_order_;
    std::vector<TestTag> tags_;
};

[[nodiscard]] std::vector<std::pair<std::int32_t, std::int32_t>> identity_matrix() {
    return {
        {1, 1}, {0, 1}, {0, 1},
        {0, 1}, {1, 1}, {0, 1},
        {0, 1}, {0, 1}, {1, 1},
    };
}

void add_minimum_profile(DcpFixtureBuilder& builder) {
    builder.add_string(50'708U, TestTiffType::ascii, "Open Camera Mk I");
    builder.add_matrix(50'721U, identity_matrix());
}

[[nodiscard]] std::vector<float> hsv_table_values(
    const std::size_t entries,
    const float nonzero_saturation_value_scale = 1.125F
) {
    std::vector<float> values;
    values.reserve(entries * 3U);
    for (std::size_t index = 0U; index < entries; ++index) {
        const bool zero_saturation = index % 2U == 0U;
        values.push_back(static_cast<float>(index) * 3.0F);
        values.push_back(zero_saturation ? 1.0F : 0.875F);
        values.push_back(zero_saturation ? 1.0F : nonzero_saturation_value_scale);
    }
    return values;
}

template <typename Configure>
[[nodiscard]] std::vector<std::byte> make_profile(
    const image::DcpByteOrder order,
    Configure configure
) {
    DcpFixtureBuilder builder(order);
    configure(builder);
    return builder.finish();
}

void complete_little_endian_profile_is_preserved() {
    const auto bytes = make_profile(
        image::DcpByteOrder::little_endian,
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            auto warm_matrix = identity_matrix();
            warm_matrix[0] = {11, 10};
            builder.add_matrix(50'722U, warm_matrix);
            builder.add_short(50'778U, 17U);
            builder.add_short(50'779U, 21U);
            builder.add_string(50'932U, TestTiffType::byte, "shadow-cal-v1");
            builder.add_string(50'936U, TestTiffType::byte, "Open Camera Standard");
            builder.add_dimensions(50'937U, 2U, 2U, 2U);
            builder.add_floats(50'938U, hsv_table_values(8U));
            builder.add_floats(50'939U, hsv_table_values(8U, 0.75F));
            builder.add_floats(
                50'940U,
                {0.0F, 0.0F, 0.5F, 0.625F, 1.0F, 1.0F}
            );
            builder.add_long(50'941U, 3U);
            builder.add_string(50'942U, TestTiffType::ascii, "Public test profile");
            builder.add_matrix(50'964U, identity_matrix());
            builder.add_matrix(50'965U, warm_matrix);
            builder.add_dimensions(50'981U, 1U, 2U, 1U);
            builder.add_floats(50'982U, hsv_table_values(2U));
            builder.add_long(51'107U, 1U);
            builder.add_signed_rational(51'109U, -7, 10);
            builder.add_long(51'110U, 1U);
        }
    );

    const auto profile = image::parse_dcp_profile(bytes);
    expect(
        profile.parse_receipt.byte_order == image::DcpByteOrder::little_endian,
        "little-endian receipt is explicit"
    );
    expect(
        profile.parse_receipt.source_bytes == bytes.size()
            && profile.parse_receipt.first_ifd_offset == 8U
            && profile.parse_receipt.ifd_entry_count == 20U,
        "bounded IFD shape is retained for audit receipts"
    );
    expect(profile.unique_camera_model == "Open Camera Mk I", "camera model is preserved");
    expect(profile.profile_name == "Open Camera Standard", "profile name is preserved");
    expect(
        profile.profile_copyright == "Public test profile",
        "profile copyright is preserved"
    );
    expect(
        profile.profile_calibration_signature == "shadow-cal-v1",
        "calibration signature is preserved"
    );
    expect(
        profile.embed_policy == image::DcpEmbedPolicy::no_restrictions
            && profile.embed_policy_was_explicit,
        "embed policy remains available to source-policy enforcement"
    );
    expect(
        profile.default_black_render == image::DcpDefaultBlackRender::none
            && profile.default_black_render_was_explicit,
        "renderer black hint is not confused with sensor black subtraction"
    );
    expect_close(
        *profile.baseline_exposure_offset_ev,
        -0.7,
        1.0e-12,
        "signed BaselineExposureOffset is retained in EV"
    );
    expect(
        profile.tone_curve.size() == 3U
            && profile.tone_curve[1].output == 0.625F,
        "tone-curve coordinate pairs are preserved"
    );
    expect(
        profile.calibration1.illuminant == 17U
            && profile.calibration1.forward_matrix.has_value()
            && profile.calibration1.hue_sat_map.has_value(),
        "first illuminant calibration remains cohesive"
    );
    expect(
        profile.calibration2.has_value()
            && profile.calibration2->illuminant == 21U
            && !profile.calibration2->color_matrix_was_inherited
            && profile.calibration2->forward_matrix.has_value()
            && profile.calibration2->hue_sat_map.has_value(),
        "second illuminant calibration is complete"
    );
    expect(
        profile.calibration1.hue_sat_map->encoding == image::DcpTableEncoding::srgb
            && profile.calibration1.hue_sat_map->entries.size() == 8U,
        "3D HueSatMap encoding and exact shape are retained"
    );
    expect(
        profile.look_table.has_value()
            && profile.look_table->encoding == image::DcpTableEncoding::linear
            && profile.look_table->entries.size() == 2U,
        "2.5D LookTable is independently represented"
    );
}

void big_endian_and_defaulted_fields_are_explicit() {
    const auto bytes = make_profile(
        image::DcpByteOrder::big_endian,
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            // Real public profiles occasionally retain this harmless scalar
            // after removing their optional LookTable.
            builder.add_long(51'108U, 1U);
        }
    );
    const auto profile = image::parse_dcp_profile(bytes);
    expect(
        profile.parse_receipt.byte_order == image::DcpByteOrder::big_endian,
        "big-endian scalar and rational values parse"
    );
    expect(
        profile.embed_policy == image::DcpEmbedPolicy::allow_copying_dng_only
            && !profile.embed_policy_was_explicit,
        "missing embed policy uses its DNG default without losing provenance"
    );
    expect(
        profile.default_black_render == image::DcpDefaultBlackRender::automatic
            && !profile.default_black_render_was_explicit,
        "missing black-render hint uses Auto without pretending it was embedded"
    );
    expect(
        profile.calibration1.illuminant == 0U
            && !profile.calibration1.illuminant_was_explicit,
        "single-illuminant unknown default is represented"
    );
}

void second_illuminant_can_inherit_color_matrix1() {
    const auto bytes = make_profile(
        image::DcpByteOrder::little_endian,
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            builder.add_short(50'778U, 17U);
            builder.add_short(50'779U, 21U);
        }
    );
    const auto profile = image::parse_dcp_profile(bytes);
    expect(
        profile.calibration2.has_value()
            && profile.calibration2->color_matrix_was_inherited
            && profile.calibration2->color_matrix
                == profile.calibration1.color_matrix,
        "ColorMatrix1 inheritance is explicit rather than silently synthesized"
    );
}

template <typename Configure>
void expect_parse_error(
    const image::DcpParseErrorCode expected,
    const std::string_view message,
    Configure configure
) {
    try {
        const auto bytes = make_profile(image::DcpByteOrder::little_endian, configure);
        static_cast<void>(image::parse_dcp_profile(bytes));
        expect(false, message);
    } catch (const image::DcpParseError& error) {
        expect(error.code() == expected, message);
    }
}

void malformed_tiff_structure_fails_closed() {
    expect_parse_error(
        image::DcpParseErrorCode::missing_required_tag,
        "ColorMatrix1 remains required",
        [](DcpFixtureBuilder& builder) {
            builder.add_string(50'708U, TestTiffType::ascii, "Missing Matrix");
        }
    );
    expect_parse_error(
        image::DcpParseErrorCode::invalid_tag_type,
        "ColorMatrix1 requires SRATIONAL",
        [](DcpFixtureBuilder& builder) {
            builder.add_string(50'708U, TestTiffType::ascii, "Wrong Matrix Type");
            builder.add_floats(
                50'721U,
                {1, 0, 0, 0, 1, 0, 0, 0, 1}
            );
        }
    );
    expect_parse_error(
        image::DcpParseErrorCode::invalid_tag_value,
        "singular matrices are rejected",
        [](DcpFixtureBuilder& builder) {
            builder.add_string(50'708U, TestTiffType::ascii, "Singular Matrix");
            builder.add_matrix(
                50'721U,
                std::vector<std::pair<std::int32_t, std::int32_t>>(9U, {0, 1})
            );
        }
    );
    expect_parse_error(
        image::DcpParseErrorCode::duplicate_or_unsorted_tag,
        "duplicate TIFF tags are rejected before semantic parsing",
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            builder.add_matrix(50'721U, identity_matrix());
        }
    );

    auto invalid_magic = make_profile(
        image::DcpByteOrder::little_endian,
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
        }
    );
    invalid_magic[2] = std::byte{42};
    try {
        static_cast<void>(image::parse_dcp_profile(invalid_magic));
        expect(false, "ordinary TIFF magic is not accepted as standalone DCP");
    } catch (const image::DcpParseError& error) {
        expect(
            error.code() == image::DcpParseErrorCode::invalid_magic,
            "ordinary TIFF magic has a stable diagnostic"
        );
    }

    auto truncated = make_profile(
        image::DcpByteOrder::little_endian,
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
        }
    );
    truncated.resize(truncated.size() - 1U);
    try {
        static_cast<void>(image::parse_dcp_profile(truncated));
        expect(false, "truncated out-of-line payload is rejected");
    } catch (const image::DcpParseError& error) {
        expect(
            error.code() == image::DcpParseErrorCode::truncated_document,
            "truncated payload has a stable diagnostic"
        );
    }
}

void malformed_curves_and_tables_fail_closed() {
    expect_parse_error(
        image::DcpParseErrorCode::invalid_tag_value,
        "tone-curve inputs must be strictly increasing",
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            builder.add_floats(
                50'940U,
                {0.0F, 0.0F, 0.75F, 0.8F, 0.5F, 0.9F, 1.0F, 1.0F}
            );
        }
    );
    expect_parse_error(
        image::DcpParseErrorCode::invalid_tag_count,
        "HueSatMap data count must exactly match dimensions",
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            builder.add_dimensions(50'937U, 1U, 2U, 1U);
            builder.add_floats(50'938U, {0.0F, 1.0F, 1.0F});
        }
    );
    expect_parse_error(
        image::DcpParseErrorCode::invalid_tag_value,
        "zero-saturation table entries require value scale one",
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            builder.add_dimensions(50'937U, 1U, 2U, 1U);
            builder.add_floats(
                50'938U,
                {0.0F, 1.0F, 0.5F, 0.0F, 1.0F, 1.0F}
            );
        }
    );
    expect_parse_error(
        image::DcpParseErrorCode::invalid_tag_value,
        "non-finite table values are rejected",
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            builder.add_dimensions(50'937U, 1U, 2U, 1U);
            builder.add_floats(
                50'938U,
                {
                    0.0F,
                    1.0F,
                    1.0F,
                    std::numeric_limits<float>::quiet_NaN(),
                    1.0F,
                    1.0F,
                }
            );
        }
    );
}

void unsupported_rendering_features_fail_loudly() {
    expect_parse_error(
        image::DcpParseErrorCode::unsupported_profile_feature,
        "third calibration is not silently reduced to two illuminants",
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            builder.add_short(52'529U, 21U);
        }
    );
    expect_parse_error(
        image::DcpParseErrorCode::unsupported_profile_feature,
        "ReductionMatrix is not silently ignored",
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            builder.add_matrix(50'725U, identity_matrix());
        }
    );
    expect_parse_error(
        image::DcpParseErrorCode::unsupported_profile_feature,
        "custom illuminants fail until their data model exists",
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            builder.add_short(50'778U, 255U);
        }
    );
    expect_parse_error(
        image::DcpParseErrorCode::unsupported_profile_feature,
        "HDR profile encoding is not treated as SDR",
        [](DcpFixtureBuilder& builder) {
            add_minimum_profile(builder);
            std::vector<std::byte> dynamic_range{
                std::byte{1}, std::byte{0},
                std::byte{1}, std::byte{0},
                std::byte{0}, std::byte{0}, std::byte{0}, std::byte{65},
            };
            builder.add_raw(
                52'551U,
                TestTiffType::undefined,
                8U,
                std::move(dynamic_range)
            );
        }
    );
}

} // namespace

int main() {
    complete_little_endian_profile_is_preserved();
    big_endian_and_defaulted_fields_are_explicit();
    second_illuminant_can_inherit_color_matrix1();
    malformed_tiff_structure_fails_closed();
    malformed_curves_and_tables_fail_closed();
    unsupported_rendering_features_fail_loudly();
    std::cout << "shadow image DCP parser contract tests passed\n";
}
