#include <shadow/image/raw_frame_staging.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace image = shadow::image;

namespace {

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("RAW frame staging contract failed: " + message);
    }
}

image::RawFrame frame_fixture() {
    image::RawFrame frame;
    frame.descriptor.provider_id = "shadow.test.raw";
    frame.descriptor.provider_version = "1.0";
    frame.descriptor.storage_dimensions = {6U, 6U};
    frame.descriptor.active_dimensions = {4U, 4U};
    frame.descriptor.active_margins = {1U, 1U, 1U, 1U};
    frame.descriptor.orientation = 0;
    frame.descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    frame.descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    frame.descriptor.cfa_pattern = "RGGB";
    frame.descriptor.bits_per_sample = 14U;
    frame.descriptor.black_levels = {64U, 65U, 66U, 67U};
    frame.descriptor.white_levels = {16'383U, 16'383U, 16'383U, 16'383U};
    frame.descriptor.as_shot_neutral = {2.0, 1.0, 1.5, 1.0};
    frame.descriptor.xyz_to_camera_d65 = {
        0.8,
        -0.2,
        -0.1,
        -0.4,
        1.2,
        0.2,
        -0.1,
        0.2,
        0.6,
    };
    frame.descriptor.has_xyz_to_camera_d65 = true;
    frame.descriptor.camera_to_linear_srgb_d65 = {
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    frame.descriptor.has_camera_to_linear_srgb_d65 = true;
    frame.samples.resize(36U);
    for (std::size_t index = 0U; index < frame.samples.size(); ++index) {
        frame.samples[index] = static_cast<std::uint16_t>(index);
    }
    return frame;
}

void active_plane_and_shifted_cfa_are_published_atomically() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root =
        fs::temp_directory_path() / ("shadow-raw-frame-staging-contract-" + std::to_string(unique));
    fs::create_directories(root);
    const auto manifest = root / "frame.shadowrawi";
    const auto receipt = image::write_raw_frame_staging(
        frame_fixture(),
        manifest,
        "01234567-89ab-cdef-0123-456789abcdef"
    );

    expect(receipt.width == 4U && receipt.height == 4U, "active dimensions");
    expect(receipt.sample_bytes == 32U, "active sample byte count");
    std::ifstream manifest_stream(manifest, std::ios::binary);
    const std::string text{
        std::istreambuf_iterator<char>(manifest_stream),
        std::istreambuf_iterator<char>(),
    };
    expect(text.starts_with("shadow-raw-frame-staging-20260806.1 "), "manifest schema");
    expect(text.find("cfa=BGGR") != std::string::npos, "active-origin CFA");
    expect(text.find("black=67,66,65,64") != std::string::npos, "active site levels");
    expect(
        text.find("provider_id_hex=736861646f772e746573742e726177") != std::string::npos,
        "provider identity"
    );

    std::ifstream sample_stream(receipt.sample_path, std::ios::binary);
    const std::vector<std::uint8_t> samples{
        std::istreambuf_iterator<char>(sample_stream),
        std::istreambuf_iterator<char>(),
    };
    expect(samples.size() == 32U, "sample file size");
    expect(samples[0] == 7U && samples[1] == 0U, "first active sample");
    expect(samples[30] == 28U && samples[31] == 0U, "last active sample");
    const auto restored = image::read_raw_frame_staging(manifest);
    expect(
        restored.descriptor.active_dimensions == image::Dimensions{4U, 4U},
        "restored dimensions"
    );
    expect(restored.descriptor.cfa_pattern == "BGGR", "restored active CFA");
    expect(
        restored.descriptor.black_levels == std::array<std::uint32_t, 4U>{67U, 66U, 65U, 64U},
        "restored site levels"
    );
    expect(restored.descriptor.has_camera_to_linear_srgb_d65, "restored camera transform");
    expect(
        restored.descriptor.has_xyz_to_camera_d65
            && restored.descriptor.xyz_to_camera_d65
                   == frame_fixture().descriptor.xyz_to_camera_d65,
        "restored physical illuminant calibration"
    );
    expect(restored.samples.front() == 7U && restored.samples.back() == 28U, "restored samples");
    expect(
        !fs::exists(manifest.string() + ".partial-01234567-89ab-cdef-0123-456789abcdef"),
        "manifest partial removed"
    );
    fs::remove_all(root);
}

} // namespace

int main() {
    active_plane_and_shifted_cfa_are_published_atomically();
    std::cout << "shadow image RAW frame staging contract tests passed\n";
}
