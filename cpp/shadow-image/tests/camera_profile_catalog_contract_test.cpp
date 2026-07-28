#include <shadow/image/camera_profile.hpp>
#include <shadow/image/camera_profile_catalog.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
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

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    expect(static_cast<bool>(output), "DCP fixture is written completely");
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
    configured_public_rawtherapee_profile_parses_when_available();
    std::cout << "shadow image camera profile catalog contract tests passed\n";
}
