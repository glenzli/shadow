#include <shadow/image/decoder.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image {

namespace {

inline constexpr std::uint32_t photo_decoder_router_contract_version = 1U;

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

class PhotoDecoderRouter final : public DecoderProvider {
public:
    PhotoDecoderRouter()
        : raw_(make_libraw_decoder_provider()), raster_(make_raster_decoder_provider()) {
        const auto& raw_info = raw_->info();
        const auto& raster_info = raster_->info();
        info_.id = "shadow-photo-router";
        info_.version = "router=" + std::to_string(photo_decoder_router_contract_version)
            + ";raw=" + compact_component_identity(raw_info.version)
            + ";raster=" + compact_component_identity(raster_info.version)
            + ";display=" + std::to_string(display_srgb8_output_transform_version);
        info_.dng_sdk = raw_info.dng_sdk;
        info_.rawspeed = raw_info.rawspeed;
        info_.jpeg = raster_info.jpeg || raw_info.jpeg;
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
        return raw_->open(path);
    }

private:
    std::unique_ptr<DecoderProvider> raw_;
    std::unique_ptr<DecoderProvider> raster_;
    ProviderInfo info_;
};

} // namespace

std::unique_ptr<DecoderProvider> make_photo_decoder_provider() {
    return std::make_unique<PhotoDecoderRouter>();
}

} // namespace shadow::image
