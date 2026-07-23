#include <shadow/image/decoder.hpp>
#include <shadow/image/private_decoder_plugin.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

inline constexpr std::uint32_t photo_decoder_router_contract_version = 1U;
inline constexpr const char* private_decoder_plugin_path_environment =
    "SHADOW_PRIVATE_DECODER_PLUGIN_PATH";
inline constexpr const char* plugin_directory_environment = "SHADOW_PLUGIN_DIRECTORY";
inline constexpr const char* disable_private_decoder_environment =
    "SHADOW_DISABLE_PRIVATE_DECODER";
inline constexpr std::string_view private_decoder_link_header =
    "shadow-private-decoder-link-v1";
inline constexpr std::string_view private_decoder_link_extension = "shadow-decoder-link";
inline constexpr std::uintmax_t maximum_private_decoder_link_bytes = 8U * 1024U;

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

[[nodiscard]] std::filesystem::path utf8_text_path(const std::string_view text) {
    std::u8string utf8_path;
    utf8_path.reserve(text.size());
    for (const char byte : text) {
        utf8_path.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path(utf8_path);
}

[[nodiscard]] std::string trim_ascii(const std::string_view text) {
    std::size_t first = 0U;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1U])) != 0) {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

[[noreturn]] void throw_private_decoder_link_error(
    const std::filesystem::path& link_path,
    const std::string_view detail
) {
    throw DecodeError(
        DecodeErrorCode::unsupported,
        0,
        "private decoder plugin link " + utf8_path_text(link_path) + ": "
            + std::string(detail)
    );
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
    return utf8_text_path(configured);
}

[[nodiscard]] bool private_decoder_plugins_enabled() noexcept {
    const auto* disabled = std::getenv(disable_private_decoder_environment);
    return disabled == nullptr || *disabled == '\0' || std::string_view(disabled) == "0";
}

[[nodiscard]] std::filesystem::path configured_plugin_root() {
    const auto* configured = std::getenv(plugin_directory_environment);
    if (configured != nullptr && *configured != '\0') {
        return utf8_text_path(configured);
    }

    const auto* home = std::getenv("HOME");
#if defined(_WIN32)
    const auto* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data != nullptr && *local_app_data != '\0') {
        return utf8_text_path(local_app_data) / "Shadow" / "plugins";
    }
    return {};
#elif defined(__APPLE__)
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return utf8_text_path(home) / "Library" / "Application Support" / "Shadow" / "plugins";
#else
    const auto* xdg_data_home = std::getenv("XDG_DATA_HOME");
    if (xdg_data_home != nullptr && *xdg_data_home != '\0') {
        return utf8_text_path(xdg_data_home) / "shadow" / "plugins";
    }
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return utf8_text_path(home) / ".local" / "share" / "shadow" / "plugins";
#endif
}

[[nodiscard]] std::filesystem::path read_private_decoder_link(
    const std::filesystem::path& link_path
) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(link_path, error) || error) {
        throw_private_decoder_link_error(link_path, "is not a readable regular file");
    }
    error.clear();
    const std::uintmax_t size = std::filesystem::file_size(link_path, error);
    if (error || size > maximum_private_decoder_link_bytes) {
        throw_private_decoder_link_error(link_path, "is too large or cannot be read");
    }

    std::ifstream file(link_path, std::ios::binary);
    std::string line;
    if (!file || !std::getline(file, line) || trim_ascii(line) != private_decoder_link_header) {
        throw_private_decoder_link_error(link_path, "does not have the expected v1 header");
    }

    std::optional<std::filesystem::path> module_path;
    while (std::getline(file, line)) {
        const std::string value = trim_ascii(line);
        if (value.empty() || value.starts_with('#')) {
            continue;
        }
        constexpr std::string_view module_prefix = "module=";
        if (!value.starts_with(module_prefix) || module_path.has_value()) {
            throw_private_decoder_link_error(link_path, "contains an invalid entry");
        }
        const std::filesystem::path module = utf8_text_path(
            trim_ascii(std::string_view(value).substr(module_prefix.size()))
        );
        if (!module.is_absolute()) {
            throw_private_decoder_link_error(link_path, "module path must be absolute");
        }
        module_path = module;
    }
    if (!module_path.has_value()) {
        throw_private_decoder_link_error(link_path, "does not declare a module path");
    }
    error.clear();
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(*module_path, error);
    const std::filesystem::path effective_path = error ? *module_path : canonical;
    error.clear();
    if (!std::filesystem::is_regular_file(effective_path, error) || error) {
        throw_private_decoder_link_error(link_path, "module path does not name a regular file");
    }
    return effective_path;
}

[[nodiscard]] std::vector<std::filesystem::path> discovered_private_decoder_plugin_paths() {
    const std::filesystem::path root = configured_plugin_root();
    if (root.empty()) {
        return {};
    }
    const std::filesystem::path decoder_directory = root / "decoders";
    std::error_code error;
    const bool directory_exists = std::filesystem::exists(decoder_directory, error);
    if (error) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "private decoder plugin directory is not accessible: "
                + utf8_path_text(decoder_directory)
        );
    }
    if (!directory_exists) {
        return {};
    }
    const bool is_directory = std::filesystem::is_directory(decoder_directory, error);
    if (error || !is_directory) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "private decoder plugin directory is not accessible: "
                + utf8_path_text(decoder_directory)
        );
    }

    std::vector<std::filesystem::path> link_paths;
    for (
        std::filesystem::directory_iterator entry(decoder_directory, error), end;
        !error && entry != end;
        entry.increment(error)
    ) {
        std::error_code entry_error;
        if (
            entry->is_regular_file(entry_error)
            && !entry_error
            && lowercase_extension(entry->path()) == private_decoder_link_extension
        ) {
            link_paths.push_back(entry->path());
        }
        if (entry_error) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                0,
                "could not inspect private decoder plugin directory entry: "
                    + utf8_path_text(entry->path())
            );
        }
    }
    if (error) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "could not enumerate private decoder plugin directory: "
                + utf8_path_text(decoder_directory)
        );
    }
    std::sort(link_paths.begin(), link_paths.end());

    std::set<std::string> seen_modules;
    std::vector<std::filesystem::path> modules;
    for (const auto& link_path : link_paths) {
        const std::filesystem::path module = read_private_decoder_link(link_path);
        if (seen_modules.insert(utf8_path_text(module)).second) {
            modules.push_back(module);
        }
    }
    return modules;
}

// A DecodeSession owns immutable source metadata, but its source-facing methods
// may enter third-party RAW libraries or a locally installed private provider.
// The catalog inspector and the editor prepare those sources on independent
// workers. Do not make them share buffers or duplicate full RAW frames merely
// to avoid that race: serialize only the source-I/O/development boundary, then
// let the immutable warm preview and detail sessions run in parallel.
class SerializedPhotoDecodeSession final : public DecodeSession {
public:
    SerializedPhotoDecodeSession(
        std::shared_ptr<std::mutex> decode_gate,
        std::unique_ptr<DecodeSession> session
    )
        : decode_gate_(std::move(decode_gate)), session_(std::move(session)) {
        if (decode_gate_ == nullptr || session_ == nullptr) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "photo router could not retain its serialized decode session"
            );
        }
    }

    [[nodiscard]] const AssetMetadata& metadata() const noexcept override {
        return session_->metadata();
    }

    [[nodiscard]] const DecodeCapabilities& capabilities() const noexcept override {
        return session_->capabilities();
    }

    [[nodiscard]] std::span<const PreviewDescriptor> previews() const noexcept override {
        return session_->previews();
    }

    [[nodiscard]] const RawDevelopmentCapabilities& raw_development_capabilities() const noexcept
        override {
        return session_->raw_development_capabilities();
    }

    [[nodiscard]] RawDevelopmentPlanNegotiation negotiate_raw_development_plan(
        const RawDevelopmentPlan& plan
    ) const noexcept override {
        return session_->negotiate_raw_development_plan(plan);
    }

    [[nodiscard]] PreviewPayload decode_preview(const std::size_t id) override {
        std::lock_guard lock(*decode_gate_);
        return session_->decode_preview(id);
    }

    [[nodiscard]] RawFrame decode_raw_frame() override {
        std::lock_guard lock(*decode_gate_);
        return session_->decode_raw_frame();
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        std::lock_guard lock(*decode_gate_);
        return session_->render_reference_rgb();
    }

    [[nodiscard]] PixelBuffer render_reference_rgb(
        const RawDevelopmentPlan& plan
    ) const override {
        std::lock_guard lock(*decode_gate_);
        return session_->render_reference_rgb(plan);
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge
    ) const override {
        std::lock_guard lock(*decode_gate_);
        return session_->render_reference_rgb_for_preview(max_edge);
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge,
        const RawDevelopmentPlan& plan
    ) const override {
        std::lock_guard lock(*decode_gate_);
        return session_->render_reference_rgb_for_preview(max_edge, plan);
    }

private:
    std::shared_ptr<std::mutex> decode_gate_;
    std::unique_ptr<DecodeSession> session_;
};

// CXX bridge operations deliberately create short-lived router providers, so a
// gate owned by one router instance would not protect an editor request from
// the catalog inspector's separate request. Keep this source-I/O gate process
// wide: provider sessions remain independent, while access to decoder engines
// that may own shared native state is serialized across the application.
[[nodiscard]] std::shared_ptr<std::mutex> shared_photo_source_decode_gate() {
    static const auto gate = std::make_shared<std::mutex>();
    return gate;
}

class PhotoDecoderRouter final : public DecoderProvider {
public:
    explicit PhotoDecoderRouter(std::vector<std::filesystem::path> private_decoder_plugin_paths)
        : decode_gate_(shared_photo_source_decode_gate()), raw_(make_libraw_decoder_provider()),
          raster_(make_raster_decoder_provider()) {
        for (const auto& private_decoder_plugin_path : private_decoder_plugin_paths) {
            private_raw_.push_back(PrivateModule{
                .path = private_decoder_plugin_path,
                .provider = load_private_decoder_plugin(private_decoder_plugin_path),
            });
        }
        const auto& raw_info = raw_->info();
        const auto& raster_info = raster_->info();
        info_.id = "shadow-photo-router";
        info_.version = "router=" + std::to_string(photo_decoder_router_contract_version)
            + ";raw=" + compact_component_identity(raw_info.version)
            + ";raster=" + compact_component_identity(raster_info.version)
            + ";display=" + std::to_string(display_srgb8_output_transform_version);
        for (const auto& module : private_raw_) {
            const auto& private_info = module.provider->info();
            info_.version += ";private=" + compact_component_identity(
                private_info.id + ";" + private_info.version
            ) + ";private_module=" + private_module_identity(module.path);
            info_.dng_sdk = info_.dng_sdk || private_info.dng_sdk;
            info_.rawspeed = info_.rawspeed || private_info.rawspeed;
            info_.jpeg = info_.jpeg || private_info.jpeg;
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
        std::lock_guard lock(*decode_gate_);
        const std::string extension = lowercase_extension(path);
        if (is_raster_extension(extension) || has_jpeg_signature(path)) {
            return std::make_unique<SerializedPhotoDecodeSession>(
                decode_gate_,
                raster_->open(path)
            );
        }

        // Public LibRaw is Shadow's normal path.  A private module is a
        // narrow escape hatch for formats that public LibRaw cannot develop
        // (for example a vendor-specific compressed RAW), not an eager
        // replacement for every RAW source.  Trying a vendor SDK first made
        // the host invoke a Nikon-only module for ordinary Canon/Sony/NEF
        // files, and a failure in that foreign SDK can take down the whole
        // desktop process before it has a chance to return `unsupported`.
        //
        // Prefer the public decoder whenever it opens a source *and* exposes
        // a usable reference-RGB path.  If it opens metadata only (the common
        // shape for an unsupported compression), keep that session as a
        // diagnostic fallback and give locally installed narrow providers a
        // chance to claim the file.  A public open failure is likewise not
        // final: the private-module contract remains able to support source
        // formats LibRaw does not recognise at all.
        std::unique_ptr<DecodeSession> public_raw_session;
        try {
            public_raw_session = raw_->open(path);
            if (public_raw_session->capabilities().reference_rgb) {
                return std::make_unique<SerializedPhotoDecodeSession>(
                    decode_gate_,
                    std::move(public_raw_session)
                );
            }
        } catch (const DecodeError&) {
            // A private provider can legitimately handle a source LibRaw
            // cannot even open, so keep probing below.  If none claims it we
            // re-open LibRaw at the end to preserve its original diagnostic.
        }

        for (const auto& module : private_raw_) {
            try {
                return std::make_unique<SerializedPhotoDecodeSession>(
                    decode_gate_,
                    module.provider->open(path)
                );
            } catch (const DecodeError& error) {
                // A private provider must explicitly say it does not recognise a source before
                // the router tries another local module or falls back. Corrupt data, a malformed
                // provider frame, an SDK licensing failure, or a resource error must remain
                // visible to the photographer instead of being hidden behind LibRaw.
                if (error.code() != DecodeErrorCode::unsupported) {
                    throw;
                }
            }
        }

        if (public_raw_session != nullptr) {
            return std::make_unique<SerializedPhotoDecodeSession>(
                decode_gate_,
                std::move(public_raw_session)
            );
        }
        return std::make_unique<SerializedPhotoDecodeSession>(decode_gate_, raw_->open(path));
    }

private:
    struct PrivateModule final {
        std::filesystem::path path;
        std::unique_ptr<DecoderProvider> provider;
    };

    std::shared_ptr<std::mutex> decode_gate_;
    std::vector<PrivateModule> private_raw_;
    std::unique_ptr<DecoderProvider> raw_;
    std::unique_ptr<DecoderProvider> raster_;
    ProviderInfo info_;
};

} // namespace

std::unique_ptr<DecoderProvider> make_photo_decoder_provider() {
    if (!private_decoder_plugins_enabled()) {
        return std::make_unique<PhotoDecoderRouter>(std::vector<std::filesystem::path>{});
    }
    const std::filesystem::path explicit_path = configured_private_decoder_plugin_path();
    if (!explicit_path.empty()) {
        return make_photo_decoder_provider(explicit_path);
    }
    return std::make_unique<PhotoDecoderRouter>(discovered_private_decoder_plugin_paths());
}

std::unique_ptr<DecoderProvider> make_photo_decoder_provider(
    const std::filesystem::path& private_decoder_plugin_path
) {
    std::vector<std::filesystem::path> paths;
    if (private_decoder_plugins_enabled() && !private_decoder_plugin_path.empty()) {
        paths.push_back(private_decoder_plugin_path);
    }
    return std::make_unique<PhotoDecoderRouter>(std::move(paths));
}

} // namespace shadow::image
