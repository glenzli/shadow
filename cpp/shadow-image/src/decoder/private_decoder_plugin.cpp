#include <shadow/image/private_decoder_plugin.hpp>

#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace shadow::image {

namespace {

[[nodiscard]] bool usable_identifier(const char* value) noexcept {
    if (value == nullptr || *value == '\0') {
        return false;
    }
    for (const auto* cursor = value; *cursor != '\0'; ++cursor) {
        const auto character = static_cast<unsigned char>(*cursor);
        if (!(std::isalnum(character) != 0 || character == '.' || character == '-' || character == '_')) {
            return false;
        }
    }
    return true;
}

[[noreturn]] void throw_plugin_error(const std::string& message) {
    throw DecodeError(DecodeErrorCode::unsupported, 0, message);
}

class PluginModule final {
public:
    explicit PluginModule(const std::filesystem::path& path) {
#if defined(_WIN32)
        handle_ = LoadLibraryW(path.c_str());
        if (handle_ == nullptr) {
            throw_plugin_error("could not load private decoder plugin");
        }
#else
        handle_ = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
        if (handle_ == nullptr) {
            throw_plugin_error(std::string("could not load private decoder plugin: ") + dlerror());
        }
#endif
        descriptor = load_symbol<PrivateDecoderPluginDescriptorFn>(
            private_decoder_plugin_descriptor_symbol
        );
        create = load_symbol<CreatePrivateDecoderProviderFn>(private_decoder_plugin_create_symbol);
        destroy = load_symbol<DestroyPrivateDecoderProviderFn>(private_decoder_plugin_destroy_symbol);
        const auto* plugin_descriptor = descriptor();
        if (plugin_descriptor == nullptr) {
            throw_plugin_error("private decoder plugin returned a null descriptor");
        }
        validate_private_decoder_plugin_descriptor(*plugin_descriptor);
    }

    PluginModule(const PluginModule&) = delete;
    PluginModule& operator=(const PluginModule&) = delete;

    ~PluginModule() {
#if defined(_WIN32)
        if (handle_ != nullptr) {
            FreeLibrary(handle_);
        }
#else
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
#endif
    }

    PrivateDecoderPluginDescriptorFn descriptor = nullptr;
    CreatePrivateDecoderProviderFn create = nullptr;
    DestroyPrivateDecoderProviderFn destroy = nullptr;

private:
    template <typename Function>
    [[nodiscard]] Function load_symbol(const char* name) {
#if defined(_WIN32)
        const auto symbol = GetProcAddress(handle_, name);
        if (symbol == nullptr) {
            throw_plugin_error("private decoder plugin is missing a required ABI symbol");
        }
        return reinterpret_cast<Function>(symbol);
#else
        dlerror();
        const auto symbol = dlsym(handle_, name);
        const auto* error = dlerror();
        if (error != nullptr || symbol == nullptr) {
            throw_plugin_error("private decoder plugin is missing a required ABI symbol");
        }
        return reinterpret_cast<Function>(symbol);
#endif
    }

#if defined(_WIN32)
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
};

class PluginDecodeSession final : public DecodeSession {
public:
    PluginDecodeSession(
        std::shared_ptr<const PluginModule> module,
        std::unique_ptr<DecodeSession> session,
        ProviderInfo provider_info
    )
        : module_(std::move(module)), session_(std::move(session)),
          provider_info_(std::move(provider_info)) {}

    [[nodiscard]] const AssetMetadata& metadata() const noexcept override {
        return session_->metadata();
    }

    [[nodiscard]] const DecodeCapabilities& capabilities() const noexcept override {
        return session_->capabilities();
    }

    [[nodiscard]] std::span<const PreviewDescriptor> previews() const noexcept override {
        return session_->previews();
    }

    [[nodiscard]] PreviewPayload decode_preview(const std::size_t id) override {
        return session_->decode_preview(id);
    }

    [[nodiscard]] MosaicBuffer decode_mosaic() override {
        return session_->decode_mosaic();
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        return bind_provider_receipt(session_->render_reference_rgb());
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge
    ) const override {
        // Do not silently fall back to DecodeSession's full-resolution default: a private
        // provider may expose a legal vendor-SDK fast path whose output has different source
        // provenance (for example half-size RAW development).
        return bind_provider_receipt(session_->render_reference_rgb_for_preview(max_edge));
    }

private:
    [[nodiscard]] PixelBuffer bind_provider_receipt(PixelBuffer pixels) const {
        auto& receipt = pixels.raw_development_receipt;
        if (!receipt.recorded()) {
            return pixels;
        }
        if (!receipt.uses_current_schema()) {
            throw_plugin_error("private decoder plugin returned an unsupported RAW receipt schema");
        }
        // The host namespaces the provider identity that reaches catalog/cache code. A plugin may
        // still name its vendor library in `library_version`, but cannot claim to be LibRaw or a
        // different private module in an auditable receipt.
        receipt.provider_id = provider_info_.id;
        receipt.provider_version = provider_info_.version;
        return pixels;
    }

    // Member order matters: session_ dies before module_, so the private implementation's vtable
    // and any SDK destructors remain mapped while the private session is destroyed.
    std::shared_ptr<const PluginModule> module_;
    std::unique_ptr<DecodeSession> session_;
    ProviderInfo provider_info_;
};

class PluginDecoderProvider final : public DecoderProvider {
public:
    PluginDecoderProvider(
        std::shared_ptr<const PluginModule> module,
        DecoderProvider* provider
    )
        : module_(std::move(module)), provider_(provider) {
        if (provider_ == nullptr) {
            throw_plugin_error("private decoder plugin returned a null provider");
        }
        info_ = provider_->info();
        const auto* descriptor = module_->descriptor();
        info_.id = "private." + std::string(descriptor->plugin_id) + "." + info_.id;
        info_.version = std::string(descriptor->plugin_version)
            + ";shadow-private-abi-v" + std::to_string(private_decoder_plugin_abi_version)
            + ";" + info_.version;
    }

    PluginDecoderProvider(const PluginDecoderProvider&) = delete;
    PluginDecoderProvider& operator=(const PluginDecoderProvider&) = delete;

    ~PluginDecoderProvider() override {
        if (provider_ != nullptr) {
            module_->destroy(provider_);
        }
    }

    [[nodiscard]] const ProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] std::unique_ptr<DecodeSession> open(
        const std::filesystem::path& path
    ) const override {
        auto session = provider_->open(path);
        if (session == nullptr) {
            throw_plugin_error("private decoder plugin returned a null session");
        }
        return std::make_unique<PluginDecodeSession>(module_, std::move(session), info_);
    }

private:
    // Provider destruction must happen while the shared module remains mapped.
    std::shared_ptr<const PluginModule> module_;
    DecoderProvider* provider_ = nullptr;
    ProviderInfo info_;
};

} // namespace

void validate_private_decoder_plugin_descriptor(
    const PrivateDecoderPluginDescriptor& descriptor
) {
    if (descriptor.abi_version != private_decoder_plugin_abi_version) {
        throw_plugin_error("private decoder plugin ABI version is unsupported");
    }
    if (!usable_identifier(descriptor.plugin_id)) {
        throw_plugin_error("private decoder plugin id is invalid");
    }
    if (descriptor.plugin_version == nullptr || *descriptor.plugin_version == '\0') {
        throw_plugin_error("private decoder plugin version is missing");
    }
}

std::unique_ptr<DecoderProvider> load_private_decoder_plugin(
    const std::filesystem::path& module_path
) {
    if (module_path.empty() || !std::filesystem::is_regular_file(module_path)) {
        throw_plugin_error("private decoder plugin path is not a regular file");
    }
    auto module = std::make_shared<PluginModule>(module_path);
    auto* provider = module->create();
    return std::make_unique<PluginDecoderProvider>(std::move(module), provider);
}

} // namespace shadow::image
