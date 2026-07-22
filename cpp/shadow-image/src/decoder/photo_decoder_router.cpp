#include <shadow/image/decoder.hpp>
#include <shadow/image/private_decoder_plugin.hpp>

#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace shadow::image {

namespace {

inline constexpr std::uint32_t photo_decoder_router_contract_version = 1U;
inline constexpr const char* private_decoder_plugin_path_environment =
    "SHADOW_PRIVATE_DECODER_PLUGIN_PATH";

[[nodiscard]] std::uint64_t fnv1a64(const std::string_view text) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= byte;
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

[[nodiscard]] std::string compact_component_identity(const std::string_view text) {
    std::ostringstream stream;
    stream << std::hex << fnv1a64(text);
    return stream.str();
}

[[nodiscard]] std::string utf8_path_text(const std::filesystem::path& path) {
    const std::u8string utf8_path = path.generic_u8string();
    std::string result;
    result.reserve(utf8_path.size());
    for (const char8_t byte : utf8_path) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

// Provider code is private and can be rebuilt in place during development. Its
// own semantic version remains the authoritative compatibility identity, but
// the module location/size/mtime fingerprint ensures an accidental rebuild
// cannot reuse photo proxies produced by an older binary in this process.
[[nodiscard]] std::string private_module_identity(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    const std::filesystem::path effective_path = error ? path : canonical;
    error.clear();
    const auto size = std::filesystem::file_size(effective_path, error);
    const std::uintmax_t effective_size = error ? 0U : size;
    error.clear();
    const auto write_time = std::filesystem::last_write_time(effective_path, error);
    const std::intmax_t ticks = error
        ? 0
        : static_cast<std::intmax_t>(write_time.time_since_epoch().count());
    return compact_component_identity(
        utf8_path_text(effective_path) + ";size=" + std::to_string(effective_size)
        + ";mtime=" + std::to_string(ticks)
    );
}

[[nodiscard]] std::string lowercase_extension(const std::filesystem::path& path) {
    const std::u8string extension = path.extension().u8string();
    if (extension.size() <= 1U) {
        return {};
    }
    std::string result;
    result.reserve(extension.size() - 1U);
    for (std::size_t index = 1U; index < extension.size(); ++index) {
        const char8_t byte = extension[index];
        result.push_back(static_cast<char>(
            byte >= u8'A' && byte <= u8'Z' ? byte - u8'A' + u8'a' : byte
        ));
    }
    return result;
}

[[nodiscard]] bool is_raster_extension(const std::string_view extension) noexcept {
    return extension == "jpg" || extension == "jpeg" || extension == "heic"
        || extension == "heif";
}

[[nodiscard]] bool has_jpeg_signature(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::array<unsigned char, 3U> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.gcount() == static_cast<std::streamsize>(bytes.size()) && bytes[0] == 0xffU
        && bytes[1] == 0xd8U && bytes[2] == 0xffU;
}

[[nodiscard]] std::filesystem::path configured_private_decoder_plugin_path() {
    const auto* configured = std::getenv(private_decoder_plugin_path_environment);
    if (configured == nullptr || *configured == '\0') {
        return {};
    }
    std::u8string utf8_path;
    for (const char byte : std::string_view(configured)) {
        utf8_path.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path(utf8_path);
}

class PhotoDecoderRouter final : public DecoderProvider {
public:
    explicit PhotoDecoderRouter(const std::filesystem::path& private_decoder_plugin_path)
        : raw_(make_libraw_decoder_provider()), raster_(make_raster_decoder_provider()) {
        if (!private_decoder_plugin_path.empty()) {
            private_raw_ = load_private_decoder_plugin(private_decoder_plugin_path);
        }
        const auto& raw_info = raw_->info();
        const auto& raster_info = raster_->info();
        info_.id = "shadow-photo-router";
        info_.version = "router=" + std::to_string(photo_decoder_router_contract_version)
            + ";raw=" + compact_component_identity(raw_info.version)
            + ";raster=" + compact_component_identity(raster_info.version)
            + ";display=" + std::to_string(display_srgb8_output_transform_version);
        if (private_raw_ != nullptr) {
            const auto& private_info = private_raw_->info();
            info_.version += ";private=" + compact_component_identity(
                private_info.id + ";" + private_info.version
            ) + ";private_module=" + private_module_identity(private_decoder_plugin_path);
            info_.dng_sdk = private_info.dng_sdk;
            info_.rawspeed = private_info.rawspeed;
            info_.jpeg = private_info.jpeg;
        }
        info_.dng_sdk = info_.dng_sdk || raw_info.dng_sdk;
        info_.rawspeed = info_.rawspeed || raw_info.rawspeed;
        info_.jpeg = info_.jpeg || raster_info.jpeg || raw_info.jpeg;
    }

    [[nodiscard]] const ProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] std::unique_ptr<DecodeSession> open(
        const std::filesystem::path& path
    ) const override {
        const std::string extension = lowercase_extension(path);
        if (is_raster_extension(extension) || has_jpeg_signature(path)) {
            return raster_->open(path);
        }
        if (private_raw_ != nullptr) {
            try {
                return private_raw_->open(path);
            } catch (const DecodeError& error) {
                // A private provider must explicitly say it does not recognise a source before
                // the router falls back. Corrupt data, a malformed provider frame, an SDK
                // licensing failure, or a resource error must remain visible to the photographer
                // instead of being hidden behind an unrelated LibRaw attempt.
                if (error.code() != DecodeErrorCode::unsupported) {
                    throw;
                }
            }
        }
        return raw_->open(path);
    }

private:
    std::unique_ptr<DecoderProvider> private_raw_;
    std::unique_ptr<DecoderProvider> raw_;
    std::unique_ptr<DecoderProvider> raster_;
    ProviderInfo info_;
};

} // namespace

std::unique_ptr<DecoderProvider> make_photo_decoder_provider() {
    return make_photo_decoder_provider(configured_private_decoder_plugin_path());
}

std::unique_ptr<DecoderProvider> make_photo_decoder_provider(
    const std::filesystem::path& private_decoder_plugin_path
) {
    return std::make_unique<PhotoDecoderRouter>(private_decoder_plugin_path);
}

} // namespace shadow::image
