#include <shadow/image/camera_profile_catalog.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace shadow::image {

namespace {

inline constexpr std::uintmax_t maximum_camera_profile_bytes = 32U * 1'024U * 1'024U;
inline constexpr std::string_view camera_profile_extension = ".dcp";

class Sha256 final {
public:
    void update(const std::span<const std::byte> input) noexcept {
        for (const std::byte value : input) {
            block_[block_bytes_++] = static_cast<std::uint8_t>(value);
            total_bytes_ += 1U;
            if (block_bytes_ == block_.size()) {
                transform(block_);
                block_bytes_ = 0U;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32U> finish() noexcept {
        const std::uint64_t bit_count = total_bytes_ * 8U;
        block_[block_bytes_++] = 0x80U;
        if (block_bytes_ > 56U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_bytes_), block_.end(), 0U);
            transform(block_);
            block_bytes_ = 0U;
        }
        std::fill(
            block_.begin() + static_cast<std::ptrdiff_t>(block_bytes_),
            block_.begin() + 56,
            0U
        );
        for (std::size_t index = 0U; index < 8U; ++index) {
            block_[63U - index] = static_cast<std::uint8_t>(bit_count >> (index * 8U));
        }
        transform(block_);

        std::array<std::uint8_t, 32U> digest{};
        for (std::size_t word = 0U; word < state_.size(); ++word) {
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                digest[word * 4U + byte] = static_cast<std::uint8_t>(
                    state_[word] >> ((3U - byte) * 8U)
                );
            }
        }
        return digest;
    }

private:
    [[nodiscard]] static constexpr std::uint32_t rotate_right(
        const std::uint32_t value,
        const std::uint32_t count
    ) noexcept {
        return (value >> count) | (value << (32U - count));
    }

    void transform(const std::array<std::uint8_t, 64U>& block) noexcept {
        static constexpr std::array<std::uint32_t, 64U> round_constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };
        std::array<std::uint32_t, 64U> schedule{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            schedule[index] =
                (static_cast<std::uint32_t>(block[index * 4U]) << 24U)
                | (static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16U)
                | (static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8U)
                | static_cast<std::uint32_t>(block[index * 4U + 3U]);
        }
        for (std::size_t index = 16U; index < schedule.size(); ++index) {
            const std::uint32_t previous15 = schedule[index - 15U];
            const std::uint32_t previous2 = schedule[index - 2U];
            const std::uint32_t sigma0 =
                rotate_right(previous15, 7U) ^ rotate_right(previous15, 18U)
                ^ (previous15 >> 3U);
            const std::uint32_t sigma1 =
                rotate_right(previous2, 17U) ^ rotate_right(previous2, 19U)
                ^ (previous2 >> 10U);
            schedule[index] = schedule[index - 16U] + sigma0 + schedule[index - 7U] + sigma1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0U; index < schedule.size(); ++index) {
            const std::uint32_t sum1 =
                rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + sum1 + choose + round_constants[index] + schedule[index];
            const std::uint32_t sum0 =
                rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8U> state_{
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };
    std::array<std::uint8_t, 64U> block_{};
    std::size_t block_bytes_ = 0U;
    std::uint64_t total_bytes_ = 0U;
};

[[nodiscard]] std::string sha256_identity(const std::span<const std::byte> content) {
    Sha256 hash;
    hash.update(content);
    const auto digest = hash.finish();
    std::ostringstream identity;
    identity << "sha256:";
    for (const std::uint8_t byte : digest) {
        identity << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned int>(byte);
    }
    return identity.str();
}

[[nodiscard]] std::string catalog_identity(
    const std::vector<CameraProfileDefinition>& profiles
) {
    Sha256 hash;
    constexpr std::string_view prefix = "shadow-camera-profile-catalog-v1";
    hash.update(std::as_bytes(std::span(prefix)));
    for (const auto& definition : profiles) {
        const std::string record = "\n" + definition.normalized_camera_model + "="
            + definition.content_identity;
        hash.update(std::as_bytes(std::span(record)));
    }
    const auto digest = hash.finish();
    std::ostringstream identity;
    identity << "shadow-camera-profile-catalog-v1:sha256:";
    for (const std::uint8_t byte : digest) {
        identity << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned int>(byte);
    }
    return identity.str();
}

[[nodiscard]] std::optional<std::vector<std::byte>> read_profile(
    const std::filesystem::path& path,
    std::string& diagnostic
) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0U || size > maximum_camera_profile_bytes
        || size > std::numeric_limits<std::size_t>::max()) {
        diagnostic = error
            ? "could not inspect the profile file"
            : "profile file is empty or exceeds the 32 MiB safety bound";
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        diagnostic = "could not open the profile file";
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        diagnostic = "could not read the complete profile file";
        return std::nullopt;
    }
    return bytes;
}

[[nodiscard]] std::filesystem::path configured_camera_profile_directory() {
    const auto* configured = std::getenv("SHADOW_CAMERA_PROFILE_DIRECTORY");
    if (configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
#if defined(_WIN32)
    const auto* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data == nullptr || *local_app_data == '\0') {
        return {};
    }
    return std::filesystem::path(local_app_data) / "Shadow" / "camera-profiles" / "dcp";
#elif defined(__APPLE__)
    const auto* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return std::filesystem::path(home) / "Library" / "Application Support" / "Shadow"
        / "camera-profiles" / "dcp";
#else
    const auto* xdg_data_home = std::getenv("XDG_DATA_HOME");
    if (xdg_data_home != nullptr && *xdg_data_home != '\0') {
        return std::filesystem::path(xdg_data_home) / "shadow" / "camera-profiles" / "dcp";
    }
    const auto* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return std::filesystem::path(home) / ".local" / "share" / "shadow"
        / "camera-profiles" / "dcp";
#endif
}

[[nodiscard]] bool starts_with_make(
    const std::string_view normalized_model,
    const std::string_view normalized_make
) noexcept {
    return normalized_model == normalized_make
        || (
            normalized_model.size() > normalized_make.size()
            && normalized_model.starts_with(normalized_make)
            && normalized_model[normalized_make.size()] == ' '
        );
}

} // namespace

std::string normalize_camera_profile_model(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    bool pending_space = false;
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        result.push_back(static_cast<char>(std::toupper(byte)));
    }
    return result;
}

std::string normalized_camera_profile_model(const AssetMetadata& metadata) {
    const std::string make = normalize_camera_profile_model(
        metadata.normalized_make.empty() ? metadata.make : metadata.normalized_make
    );
    const std::string model = normalize_camera_profile_model(
        metadata.normalized_model.empty() ? metadata.model : metadata.normalized_model
    );
    if (make.empty()) {
        return model;
    }
    if (model.empty() || starts_with_make(model, make)) {
        return model;
    }
    return make + " " + model;
}

CameraProfileCatalog load_camera_profile_catalog(const std::filesystem::path& directory) {
    CameraProfileCatalog catalog;
    std::error_code error;
    if (directory.empty() || !std::filesystem::is_directory(directory, error) || error) {
        catalog.diagnostics.push_back(CameraProfileDiagnostic{
            .code = CameraProfileDiagnosticCode::directory_unavailable,
            .message = "camera profile directory is unavailable",
        });
        catalog.identity = catalog_identity(catalog.profiles);
        return catalog;
    }

    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        const auto& entry = *iterator;
        if (!entry.is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const char character) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        });
        if (extension == camera_profile_extension) {
            candidates.push_back(entry.path());
        }
    }
    if (error) {
        catalog.diagnostics.push_back(CameraProfileDiagnostic{
            .code = CameraProfileDiagnosticCode::directory_iteration_failed,
            .message = "camera profile directory could not be enumerated completely",
        });
    }
    std::sort(candidates.begin(), candidates.end());

    for (const auto& path : candidates) {
        std::string read_diagnostic;
        const auto bytes = read_profile(path, read_diagnostic);
        if (!bytes.has_value()) {
            catalog.diagnostics.push_back(CameraProfileDiagnostic{
                .code = CameraProfileDiagnosticCode::file_read_failed,
                .source_name = path.filename().string(),
                .message = std::move(read_diagnostic),
            });
            continue;
        }
        try {
            DcpProfile profile = parse_dcp_profile(*bytes);
            const std::string normalized_model =
                normalize_camera_profile_model(profile.unique_camera_model);
            if (normalized_model.empty()) {
                catalog.diagnostics.push_back(CameraProfileDiagnostic{
                    .code = CameraProfileDiagnosticCode::profile_parse_failed,
                    .source_name = path.filename().string(),
                    .message = "DCP UniqueCameraModel becomes empty after normalization",
                });
                continue;
            }
            const bool duplicate = std::ranges::any_of(
                catalog.profiles,
                [&normalized_model](const CameraProfileDefinition& definition) {
                    return definition.normalized_camera_model == normalized_model;
                }
            );
            if (duplicate) {
                catalog.diagnostics.push_back(CameraProfileDiagnostic{
                    .code = CameraProfileDiagnosticCode::duplicate_camera_model,
                    .source_name = path.filename().string(),
                    .message = "another profile already owns this exact normalized camera model",
                });
                continue;
            }
            catalog.profiles.push_back(CameraProfileDefinition{
                .profile = std::move(profile),
                .normalized_camera_model = normalized_model,
                .content_identity = sha256_identity(*bytes),
                .source_name = path.filename().string(),
            });
        } catch (const DcpParseError& parse_error) {
            std::ostringstream diagnostic;
            diagnostic << parse_error.what() << " at byte " << parse_error.byte_offset();
            if (parse_error.tag().has_value()) {
                diagnostic << " (tag " << *parse_error.tag() << ')';
            }
            catalog.diagnostics.push_back(CameraProfileDiagnostic{
                .code = CameraProfileDiagnosticCode::profile_parse_failed,
                .source_name = path.filename().string(),
                .message = diagnostic.str(),
            });
        }
    }
    std::sort(
        catalog.profiles.begin(),
        catalog.profiles.end(),
        [](const CameraProfileDefinition& left, const CameraProfileDefinition& right) {
            return left.normalized_camera_model < right.normalized_camera_model;
        }
    );
    catalog.identity = catalog_identity(catalog.profiles);
    return catalog;
}

CameraProfileCatalog load_local_camera_profile_catalog() {
    return load_camera_profile_catalog(configured_camera_profile_directory());
}

const CameraProfileCatalog& default_camera_profile_catalog() {
    static const CameraProfileCatalog catalog = load_local_camera_profile_catalog();
    return catalog;
}

const CameraProfileDefinition* match_camera_profile(
    const CameraProfileCatalog& catalog,
    const AssetMetadata& metadata
) {
    if (catalog.schema_version != camera_profile_catalog_schema_version) {
        return nullptr;
    }
    const std::string model = normalized_camera_profile_model(metadata);
    if (model.empty()) {
        return nullptr;
    }
    const auto match = std::lower_bound(
        catalog.profiles.begin(),
        catalog.profiles.end(),
        model,
        [](const CameraProfileDefinition& definition, const std::string_view key) {
            return definition.normalized_camera_model < key;
        }
    );
    return match != catalog.profiles.end() && match->normalized_camera_model == model
        ? std::addressof(*match)
        : nullptr;
}

} // namespace shadow::image
