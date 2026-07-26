#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/fused_raw_development.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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

void write_u16(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const std::uint16_t value
) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_u32(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const std::uint32_t value
) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

void write_i32(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const std::int32_t value
) {
    write_u32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

struct FixtureTag final {
    std::uint16_t id = 0U;
    std::uint16_t type = 0U;
    std::uint32_t count = 0U;
    std::vector<std::byte> payload;
};

[[nodiscard]] std::vector<std::byte> rational_matrix(
    const std::array<std::int32_t, 9U>& numerators,
    const std::int32_t denominator
) {
    std::vector<std::byte> payload(9U * 8U);
    for (std::size_t index = 0U; index < numerators.size(); ++index) {
        write_i32(payload, index * 8U, numerators[index]);
        write_i32(payload, index * 8U + 4U, denominator);
    }
    return payload;
}

[[nodiscard]] std::vector<std::byte> minimal_dcp(
    const std::string_view camera_model,
    const std::int32_t baseline_exposure_tenths = 0
) {
    std::vector<std::byte> model;
    for (const char character : camera_model) {
        model.push_back(static_cast<std::byte>(character));
    }
    model.push_back(std::byte{0});

    const std::array<std::int32_t, 9U> identity{
        10'000, 0, 0,
        0, 10'000, 0,
        0, 0, 10'000,
    };
    std::vector<std::byte> baseline(8U);
    write_i32(baseline, 0U, baseline_exposure_tenths);
    write_i32(baseline, 4U, 10);
    std::vector<FixtureTag> tags{
        FixtureTag{50'708U, 2U, static_cast<std::uint32_t>(model.size()), std::move(model)},
        FixtureTag{50'721U, 10U, 9U, rational_matrix(identity, 10'000)},
        FixtureTag{51'109U, 10U, 1U, std::move(baseline)},
    };
    const std::size_t ifd_bytes = 2U + tags.size() * 12U + 4U;
    std::vector<std::byte> bytes(8U + ifd_bytes, std::byte{0});
    bytes[0] = static_cast<std::byte>('I');
    bytes[1] = static_cast<std::byte>('I');
    write_u16(bytes, 2U, 0x4352U);
    write_u32(bytes, 4U, 8U);
    write_u16(bytes, 8U, static_cast<std::uint16_t>(tags.size()));
    for (std::size_t index = 0U; index < tags.size(); ++index) {
        const auto& tag = tags[index];
        const std::size_t entry = 10U + index * 12U;
        write_u16(bytes, entry, tag.id);
        write_u16(bytes, entry + 2U, tag.type);
        write_u32(bytes, entry + 4U, tag.count);
        while (bytes.size() % 4U != 0U) {
            bytes.push_back(std::byte{0});
        }
        write_u32(bytes, entry + 8U, static_cast<std::uint32_t>(bytes.size()));
        bytes.insert(bytes.end(), tag.payload.begin(), tag.payload.end());
    }
    return bytes;
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count();
        path = std::filesystem::temp_directory_path()
            / ("shadow-camera-profile-test-" + std::to_string(suffix));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

class ScopedEnvironment final {
public:
    ScopedEnvironment(const char* name, const char* value)
        : name_(name) {
        if (const auto* current = std::getenv(name_); current != nullptr) {
            previous_ = current;
        }
        expect(
            ::setenv(name_, value, 1) == 0,
            "test execution backend environment is configured"
        );
    }

    ~ScopedEnvironment() {
        if (previous_.has_value()) {
            static_cast<void>(::setenv(name_, previous_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name_));
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    const char* name_;
    std::optional<std::string> previous_;
};

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    expect(static_cast<bool>(output), "DCP fixture is written completely");
}

[[nodiscard]] image::DcpMatrix3x3 diagonal_matrix(
    const double first,
    const double second,
    const double third
) {
    return image::DcpMatrix3x3{{
        first, 0.0, 0.0,
        0.0, second, 0.0,
        0.0, 0.0, third,
    }};
}

[[nodiscard]] image::CameraProfileDefinition profile_definition(
    const bool with_forward_matrix,
    const double exposure_offset = 0.0
) {
    image::DcpProfile profile;
    profile.unique_camera_model = "OPEN CAMERA V1";
    profile.profile_name = "Open Camera Standard";
    profile.calibration1.illuminant = 21U;
    profile.calibration1.illuminant_was_explicit = true;
    profile.calibration1.color_matrix = diagonal_matrix(
        1.0 / 0.95047,
        1.0,
        1.0 / 1.08883
    );
    if (with_forward_matrix) {
        profile.calibration1.forward_matrix = diagonal_matrix(
            0.964295676,
            1.0,
            0.825104603
        );
    }
    profile.baseline_exposure_offset_ev = exposure_offset;
    return image::CameraProfileDefinition{
        .profile = std::move(profile),
        .normalized_camera_model = "OPEN CAMERA V1",
        .content_identity = "sha256:test-profile",
        .source_name = "open-camera-v1.dcp",
    };
}

[[nodiscard]] image::RawFrameDescriptor raw_descriptor() {
    image::RawFrameDescriptor descriptor;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
    return descriptor;
}

void catalog_is_content_addressed_and_matches_exactly() {
    TemporaryDirectory directory;
    const auto first_bytes = minimal_dcp("NIKON   Z  9");
    write_file(directory.path / "a.dcp", first_bytes);
    write_file(directory.path / "ignored.txt", first_bytes);
    write_file(directory.path / "broken.dcp", {std::byte{1}, std::byte{2}});

    const auto catalog = image::load_camera_profile_catalog(directory.path);
    expect(catalog.profiles.size() == 1U, "one valid DCP is loaded");
    expect(
        catalog.profiles.front().normalized_camera_model == "NIKON Z 9",
        "DCP camera model uses stable case and whitespace normalization"
    );
    expect(
        catalog.profiles.front().content_identity.starts_with("sha256:")
            && catalog.profiles.front().content_identity.size() == 71U,
        "complete DCP bytes receive a SHA-256 content identity"
    );
    expect(
        std::ranges::any_of(
            catalog.diagnostics,
            [](const image::CameraProfileDiagnostic& diagnostic) {
                return diagnostic.code
                    == image::CameraProfileDiagnosticCode::profile_parse_failed;
            }
        ),
        "invalid optional DCP becomes a diagnostic"
    );

    image::AssetMetadata metadata;
    metadata.normalized_make = "Nikon";
    metadata.normalized_model = "Z 9";
    const auto match = image::match_camera_profile(catalog, metadata);
    expect(
        match != nullptr && match->content_identity == catalog.profiles.front().content_identity,
        "normalized make and model match the exact DCP UniqueCameraModel"
    );
    metadata.normalized_model = "Z 8";
    expect(
        image::match_camera_profile(catalog, metadata) == nullptr,
        "nearby camera names never fuzzy-match"
    );

    write_file(directory.path / "a.dcp", minimal_dcp("NIKON Z 9", 5));
    const auto modified = image::load_camera_profile_catalog(directory.path);
    expect(
        modified.profiles.front().content_identity
            != catalog.profiles.front().content_identity
            && modified.identity != catalog.identity,
        "modifying profile content in place changes both profile and catalog identity"
    );
}

void duplicate_exact_models_are_diagnostic_and_deterministic() {
    TemporaryDirectory directory;
    write_file(directory.path / "a.dcp", minimal_dcp("Open Camera"));
    write_file(directory.path / "b.dcp", minimal_dcp(" open   camera ", 5));
    const auto catalog = image::load_camera_profile_catalog(directory.path);
    expect(catalog.profiles.size() == 1U, "duplicate normalized camera model is not ambiguous");
    expect(
        catalog.profiles.front().source_name == "a.dcp",
        "lexically first exact profile wins deterministically"
    );
    expect(
        std::ranges::any_of(
            catalog.diagnostics,
            [](const image::CameraProfileDiagnostic& diagnostic) {
                return diagnostic.code
                    == image::CameraProfileDiagnosticCode::duplicate_camera_model;
            }
        ),
        "duplicate normalized camera model is visible"
    );
}

void forward_matrix_is_preferred_and_superwhite_is_preserved() {
    const auto transform = image::compile_dcp_color_transform(
        profile_definition(true, 1.0),
        raw_descriptor()
    );
    expect(
        transform.receipt.matrix_route == image::DcpMatrixRoute::forward_matrix,
        "valid ForwardMatrix is preferred"
    );
    const auto white = transform.apply({1.0, 1.0, 1.0});
    expect_close(white[0], 2.0, 2.0e-4, "baseline exposure scales neutral red");
    expect_close(white[1], 2.0, 2.0e-4, "baseline exposure scales neutral green");
    expect_close(white[2], 2.0, 2.0e-4, "baseline exposure scales neutral blue");
    expect(
        white[0] > 1.0 && white[1] > 1.0 && white[2] > 1.0,
        "DCP developer does not clip super-white values"
    );
}

void color_matrix_is_inverted_and_adapted() {
    const auto transform = image::compile_dcp_color_transform(
        profile_definition(false),
        raw_descriptor()
    );
    expect(
        transform.receipt.matrix_route == image::DcpMatrixRoute::inverse_color_matrix,
        "ColorMatrix fallback records the explicit inverse route"
    );
    const auto white = transform.apply({1.0, 1.0, 1.0});
    expect_close(white[0], 1.0, 2.0e-4, "D65 neutral remains neutral red");
    expect_close(white[1], 1.0, 2.0e-4, "D65 neutral remains neutral green");
    expect_close(white[2], 1.0, 2.0e-4, "D65 neutral remains neutral blue");
}

void dual_illuminant_interpolation_is_deterministic() {
    auto definition = profile_definition(false);
    definition.profile.calibration1.illuminant = 23U;
    definition.profile.calibration1.color_matrix = diagonal_matrix(
        1.0 / 0.964295676,
        1.0,
        1.0 / 0.825104603
    );
    image::DcpIlluminantCalibration second;
    second.illuminant = 21U;
    second.illuminant_was_explicit = true;
    second.color_matrix = diagonal_matrix(
        1.0 / 0.95047,
        1.0,
        1.0 / 1.08883
    );
    definition.profile.calibration2 = second;
    const auto first = image::compile_dcp_color_transform(definition, raw_descriptor());
    const auto second_result = image::compile_dcp_color_transform(definition, raw_descriptor());
    expect(
        first.receipt.calibration1_weight > 0.0
            && first.receipt.calibration1_weight < 1.0,
        "as-shot neutral resolves an interior reciprocal-temperature blend"
    );
    expect(
        first.receipt == second_result.receipt
            && first.camera_to_linear_srgb_d65 == second_result.camera_to_linear_srgb_d65,
        "dual-illuminant solve is deterministic"
    );
}

[[nodiscard]] image::DcpHsvTable value_scale_table(const float saturated_value_scale) {
    return image::DcpHsvTable{
        .hue_divisions = 1U,
        .saturation_divisions = 2U,
        .value_divisions = 1U,
        .encoding = image::DcpTableEncoding::linear,
        .entries = {
            image::DcpHsvDelta{.hue_shift_degrees = 0.0F, .saturation_scale = 1.0F, .value_scale = 1.0F},
            image::DcpHsvDelta{.hue_shift_degrees = 0.0F, .saturation_scale = 1.0F, .value_scale = saturated_value_scale},
        },
    };
}

[[nodiscard]] image::PixelBuffer one_linear_srgb_pixel(
    const std::uint16_t red,
    const std::uint16_t green,
    const std::uint16_t blue
) {
    image::PixelBuffer pixel;
    pixel.dimensions = image::Dimensions{1U, 1U};
    pixel.bits_per_channel = 16U;
    pixel.channels = 3U;
    pixel.row_stride_bytes = 3U * sizeof(std::uint16_t);
    pixel.primaries = image::RgbPrimaries::srgb_rec709_d65;
    pixel.transfer_function = image::RgbTransferFunction::linear;
    pixel.reference = image::RgbBufferReference::processed_raw;
    pixel.samples = {red, green, blue};
    return pixel;
}

[[nodiscard]] image::SceneLinearRgbFrame one_scene_linear_srgb_pixel(
    const float red,
    const float green,
    const float blue
) {
    return image::SceneLinearRgbFrame{
        .dimensions = image::Dimensions{1U, 1U},
        .row_stride_bytes = 3U * sizeof(float),
        .samples = {red, green, blue},
    };
}

void standard_dcp_rendering_stages_compile_and_apply() {
    auto definition = profile_definition(true);
    definition.profile.calibration1.hue_sat_map = value_scale_table(0.8F);
    definition.profile.look_table = value_scale_table(0.7F);
    definition.profile.tone_curve = {
        image::DcpToneCurvePoint{0.0F, 0.0F},
        image::DcpToneCurvePoint{0.5F, 0.35F},
        image::DcpToneCurvePoint{1.0F, 1.0F},
    };
    const auto transform = image::compile_dcp_color_transform(definition, raw_descriptor());
    expect(
        transform.valid() && transform.has_post_matrix_stages()
            && transform.receipt.hue_sat_map_applied
            && transform.receipt.look_table_applied
            && transform.receipt.tone_curve_applied,
        "standard DCP input-rendering stages compile into one valid camera transform"
    );
    auto pixel = one_linear_srgb_pixel(60'000U, 20'000U, 2'000U);
    static_cast<void>(image::apply_dcp_color_rendering_stages(pixel, transform));
    expect(
        pixel.samples[0] < 60'000U && pixel.samples[1] < 20'000U,
        "HueSatMap, LookTable, and ToneCurve affect camera rendering before the edit graph"
    );
    const std::string receipt_identity = image::dcp_color_receipt_identity(transform.receipt);
    expect(
        receipt_identity.find("huesat=applied") != std::string::npos
            && receipt_identity.find("look=applied") != std::string::npos
            && receipt_identity.find("tone=applied") != std::string::npos,
        "camera-rendering cache identity records every applied DCP stage"
    );
}

void scene_linear_dcp_stages_preserve_highlight_headroom() {
    auto definition = profile_definition(true);
    definition.profile.calibration1.hue_sat_map = value_scale_table(1.0F);
    const auto transform = image::compile_dcp_color_transform(definition, raw_descriptor());
    auto pixel = one_scene_linear_srgb_pixel(1.5F, 1.0F, 0.5F);
    static_cast<void>(image::apply_dcp_color_rendering_stages(pixel, transform));
    expect(
        pixel.valid() && pixel.samples[0] > 1.35F,
        "scene-linear DCP rendering keeps super-white RAW headroom"
    );
    expect_close(
        static_cast<double>(pixel.samples[1] / pixel.samples[0]),
        2.0 / 3.0,
        2.0e-3,
        "identity DCP HueSatMap preserves the normalized HDR colour ratio"
    );
}

void large_scene_linear_dcp_stage_matches_the_single_pixel_reference() {
    auto definition = profile_definition(true);
    definition.profile.calibration1.hue_sat_map = value_scale_table(0.8F);
    definition.profile.look_table = value_scale_table(0.7F);
    definition.profile.tone_curve = {
        image::DcpToneCurvePoint{0.0F, 0.0F},
        image::DcpToneCurvePoint{0.5F, 0.35F},
        image::DcpToneCurvePoint{1.0F, 1.0F},
    };
    const auto transform = image::compile_dcp_color_transform(definition, raw_descriptor());
    auto expected = one_scene_linear_srgb_pixel(0.91F, 0.37F, 0.08F);
    static_cast<void>(image::apply_dcp_color_rendering_stages(expected, transform));

    // This exceeds the DCP work partitioning threshold. Every pixel begins
    // with exactly the same data, so any split/exception ordering issue would
    // be visible as a result that differs from the known single-pixel oracle.
    image::SceneLinearRgbFrame frame{
        .dimensions = image::Dimensions{256U, 192U},
        .row_stride_bytes = 256U * 3U * sizeof(float),
    };
    frame.samples.reserve(static_cast<std::size_t>(frame.dimensions.width)
        * frame.dimensions.height * 3U);
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(frame.dimensions.width) * frame.dimensions.height;
         ++pixel) {
        frame.samples.insert(frame.samples.end(), {0.91F, 0.37F, 0.08F});
    }
    static_cast<void>(image::apply_dcp_color_rendering_stages(frame, transform));
    expect(frame.valid(), "large scene-linear DCP result retains the frame contract");
    for (std::size_t index = 0U; index < frame.samples.size(); ++index) {
        expect_close(
            frame.samples[index],
            expected.samples[index % 3U],
            1.0e-6,
            "parallel DCP post stages preserve the serial per-pixel result"
        );
    }
}

void scene_linear_dcp_metal_matches_the_cpu_reference_when_available() {
    expect(
        image::dcp_color_execution_backend_identity(image::DcpColorExecutionBackend::cpu)
            == "dcp-executor=cpu-v1;math=f64-reference",
        "DCP cache identity identifies the CPU numerical reference"
    );
    expect(
        image::dcp_color_execution_backend_identity(image::DcpColorExecutionBackend::metal)
            == "dcp-executor=metal-v1;math=f32",
        "DCP cache identity identifies the Metal executor"
    );
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        return;
    }

    auto definition = profile_definition(true);
    definition.profile.calibration1.hue_sat_map = value_scale_table(0.82F);
    definition.profile.look_table = value_scale_table(0.73F);
    definition.profile.tone_curve = {
        image::DcpToneCurvePoint{0.0F, 0.0F},
        image::DcpToneCurvePoint{0.5F, 0.31F},
        image::DcpToneCurvePoint{1.0F, 1.0F},
    };
    const auto transform = image::compile_dcp_color_transform(definition, raw_descriptor());
    image::SceneLinearRgbFrame cpu{
        .dimensions = image::Dimensions{5U, 1U},
        .row_stride_bytes = 5U * 3U * sizeof(float),
        .samples = {
            0.91F, 0.37F, 0.08F,
            0.18F, 0.63F, 0.92F,
            1.50F, 0.70F, 0.30F,
            -0.12F, 0.30F, 0.70F,
            1.80F, 1.10F, 0.40F,
        },
    };
    auto metal = cpu;
    {
        ScopedEnvironment backend("SHADOW_IMAGE_ACCELERATION", "cpu");
        expect(
            image::apply_dcp_color_rendering_stages(cpu, transform)
                == image::DcpColorExecutionBackend::cpu,
            "DCP CPU mode stays on the reference executor"
        );
    }
    {
        ScopedEnvironment backend("SHADOW_IMAGE_ACCELERATION", "metal");
        expect(
            image::apply_dcp_color_rendering_stages(metal, transform)
                == image::DcpColorExecutionBackend::metal,
            "DCP Metal mode dispatches the scene-linear GPU executor"
        );
    }
    expect(cpu.valid() && metal.valid(), "CPU and Metal DCP frames retain their contracts");
    expect(cpu.samples.size() == metal.samples.size(), "CPU and Metal DCP output shapes agree");
    for (std::size_t index = 0U; index < cpu.samples.size(); ++index) {
        expect_close(
            metal.samples[index],
            cpu.samples[index],
            2.5e-4,
            "Metal DCP agrees with the CPU reference within fp32 tolerance"
        );
    }
}

void configured_public_rawtherapee_profile_parses_when_available() {
    const auto* configured = std::getenv("SHADOW_TEST_RAWTHERAPEE_DCP_PROFILE");
    if (configured == nullptr || *configured == '\0') {
        return;
    }
    const auto profile = image::load_dcp_profile(configured);
    expect(
        !profile.unique_camera_model.empty(),
        "configured public RawTherapee DCP parses with a camera identity"
    );
}

} // namespace

int main() {
    catalog_is_content_addressed_and_matches_exactly();
    duplicate_exact_models_are_diagnostic_and_deterministic();
    forward_matrix_is_preferred_and_superwhite_is_preserved();
    color_matrix_is_inverted_and_adapted();
    dual_illuminant_interpolation_is_deterministic();
    standard_dcp_rendering_stages_compile_and_apply();
    scene_linear_dcp_stages_preserve_highlight_headroom();
    large_scene_linear_dcp_stage_matches_the_single_pixel_reference();
    scene_linear_dcp_metal_matches_the_cpu_reference_when_available();
    configured_public_rawtherapee_profile_parses_when_available();
    std::cout << "shadow image camera profile contract tests passed\n";
}
