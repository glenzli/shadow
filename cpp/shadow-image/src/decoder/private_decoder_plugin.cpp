#include <shadow/image/private_decoder_plugin.hpp>

#include <shadow/image/decoder_error.hpp>

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
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

[[nodiscard]] std::uint64_t fnv1a64(const std::string_view text) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

[[nodiscard]] std::string compact_identity(const std::string_view text) {
    std::ostringstream stream;
    stream << std::hex << fnv1a64(text);
    return stream.str();
}

// A private module is commonly rebuilt in place during local development.
// Keep the binary fingerprint beside the plugin-declared semantic version so
// a RawFrame cannot retain a stale sensor/development cache identity merely
// because its author forgot to bump a local version string.
[[nodiscard]] std::string private_module_binary_identity(const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    const auto effective_path = error ? path : canonical;
    error.clear();
    const auto size = std::filesystem::file_size(effective_path, error);
    const auto effective_size = error ? std::uintmax_t{0U} : size;
    error.clear();
    const auto write_time = std::filesystem::last_write_time(effective_path, error);
    const auto ticks = error
        ? std::intmax_t{0}
        : static_cast<std::intmax_t>(write_time.time_since_epoch().count());
    return compact_identity(
        effective_path.generic_string() + ";size=" + std::to_string(effective_size)
        + ";mtime=" + std::to_string(ticks)
    );
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
        try {
            // This is the only plugin code called before the C++ boundary is trusted. It has a
            // scalar C ABI and therefore cannot dereference a stale descriptor or construct an
            // object whose vtable follows older Shadow headers.
            interface_contract = load_symbol<PrivateDecoderPluginInterfaceContractFn>(
                private_decoder_plugin_interface_contract_symbol
            );
            validate_private_decoder_plugin_interface_contract(interface_contract());

            descriptor = load_symbol<PrivateDecoderPluginDescriptorFn>(
                private_decoder_plugin_descriptor_symbol
            );
            const auto* plugin_descriptor = descriptor();
            if (plugin_descriptor == nullptr) {
                throw_plugin_error("private decoder plugin returned a null descriptor");
            }
            validate_private_decoder_plugin_descriptor(*plugin_descriptor);

            create = load_symbol<CreatePrivateDecoderProviderFn>(
                private_decoder_plugin_create_symbol
            );
            destroy = load_symbol<DestroyPrivateDecoderProviderFn>(
                private_decoder_plugin_destroy_symbol
            );
        } catch (...) {
            close();
            throw;
        }
    }

    PluginModule(const PluginModule&) = delete;
    PluginModule& operator=(const PluginModule&) = delete;

    ~PluginModule() {
        close();
    }

    PrivateDecoderPluginInterfaceContractFn interface_contract = nullptr;
    PrivateDecoderPluginDescriptorFn descriptor = nullptr;
    CreatePrivateDecoderProviderFn create = nullptr;
    DestroyPrivateDecoderProviderFn destroy = nullptr;

private:
    void close() noexcept {
#if defined(_WIN32)
        if (handle_ != nullptr) {
            FreeLibrary(handle_);
            handle_ = nullptr;
        }
#else
        if (handle_ != nullptr) {
            dlclose(handle_);
            handle_ = nullptr;
        }
#endif
    }

    template <typename Function>
    [[nodiscard]] Function load_symbol(const char* name) {
#if defined(_WIN32)
        const auto symbol = GetProcAddress(handle_, name);
        if (symbol == nullptr) {
            throw_plugin_error(
                std::string("private decoder plugin is missing required ABI symbol: ") + name
            );
        }
        return reinterpret_cast<Function>(symbol);
#else
        dlerror();
        const auto symbol = dlsym(handle_, name);
        const auto* error = dlerror();
        if (error != nullptr || symbol == nullptr) {
            throw_plugin_error(
                std::string("private decoder plugin is missing required ABI symbol: ") + name
            );
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
        return session_->decode_preview(id);
    }

    [[nodiscard]] RawFrame decode_raw_frame() override {
        auto frame = session_->decode_raw_frame();
        // RawFrame sensor/detail cache keys must name the exact host-facing
        // private module identity, not an arbitrary provider-local string.
        // This mirrors receipt binding for rendered RGB and prevents a rebuilt
        // local decoder from silently reusing its previous raw-development
        // products.
        const std::string plugin_frame_identity = frame.descriptor.provider_id
            + ";" + frame.descriptor.provider_version;
        frame.descriptor.provider_id = provider_info_.id;
        frame.descriptor.provider_version = provider_info_.version
            + ";frame=" + compact_identity(plugin_frame_identity);
        if (raw_development_capabilities().raw_frame && !frame.valid()) {
            throw_plugin_error("private decoder plugin returned an invalid RAW frame");
        }
        return frame;
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        return bind_provider_receipt(session_->render_reference_rgb());
    }

    [[nodiscard]] PixelBuffer render_reference_rgb(
        const RawDevelopmentPlan& plan
    ) const override {
        const auto negotiation = require_accepted_plan(plan, false);
        return bind_provider_receipt(
            session_->render_reference_rgb(plan),
            &negotiation
        );
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge
    ) const override {
        // Do not silently fall back to DecodeSession's full-resolution default: a private
        // provider may expose a legal vendor-SDK fast path whose output has different source
        // provenance (for example half-size RAW development).
        return bind_provider_receipt(session_->render_reference_rgb_for_preview(max_edge));
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge,
        const RawDevelopmentPlan& plan
    ) const override {
        const auto negotiation = require_accepted_plan(plan, true);
        return bind_provider_receipt(
            session_->render_reference_rgb_for_preview(max_edge, plan),
            &negotiation
        );
    }

private:
    [[nodiscard]] RawDevelopmentPlanNegotiation require_accepted_plan(
        const RawDevelopmentPlan& plan,
        const bool preview_render
    ) const {
        const auto negotiation = session_->negotiate_raw_development_plan(plan);
        if (!negotiation.accepted()) {
            throw_plugin_error("private decoder plugin cannot satisfy the requested RAW development plan");
        }
        if (
            preview_render
                != (negotiation.effective.intent == RawDevelopmentIntent::preview)
        ) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                preview_render
                    ? "private decoder preview requires a preview RAW development plan"
                    : "private decoder full render requires a detail or export RAW development plan"
            );
        }
        return negotiation;
    }

    [[nodiscard]] PixelBuffer bind_provider_receipt(
        PixelBuffer pixels,
        const RawDevelopmentPlanNegotiation* expected_plan = nullptr
    ) const {
        auto& receipt = pixels.raw_development_receipt;
        if (!receipt.recorded()) {
            if (
                expected_plan != nullptr
                && raw_development_capabilities().available
            ) {
                throw_plugin_error(
                    "plan-aware private decoder plugin returned RAW pixels without a receipt"
                );
            }
            return pixels;
        }
        if (!receipt.uses_current_schema()) {
            throw_plugin_error("private decoder plugin returned an unsupported RAW receipt schema");
        }
        try {
            if (
                receipt.requested_plan_identity
                    != raw_development_plan_identity(receipt.requested_plan)
                || receipt.effective_plan_identity
                    != raw_development_plan_identity(receipt.effective_plan)
            ) {
                throw_plugin_error(
                    "private decoder plugin returned a non-canonical RAW development plan receipt"
                );
            }
        } catch (const std::invalid_argument&) {
            throw_plugin_error("private decoder plugin returned an invalid RAW development plan");
        }
        if (expected_plan != nullptr) {
            if (
                receipt.requested_plan != expected_plan->requested
                || receipt.effective_plan != expected_plan->effective
                || receipt.plan_negotiation_status != expected_plan->status
            ) {
                throw_plugin_error(
                    "private decoder plugin receipt does not match the negotiated RAW development plan"
                );
            }
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
        DecoderProvider* provider,
        std::string module_binary_identity
    )
        : module_(std::move(module)), provider_(provider) {
        if (provider_ == nullptr) {
            throw_plugin_error("private decoder plugin returned a null provider");
        }
        info_ = provider_->info();
        const auto* descriptor = module_->descriptor();
        info_.id = "private." + std::string(descriptor->plugin_id) + "." + info_.id;
        // ProviderInfo::version is persisted in cache identities and deliberately capped. Its
        // compact fields are a=ABI, c=interface seal, p=plan, f=RawFrame, w=wrapped identity and
        // m=module binary. An adapter such as the LibRaw dummy may wrap an already verbose
        // provider signature, so preserve its complete identity through a stable compact hash
        // instead of rejecting a valid local module merely for being descriptive.
        const std::string wrapped_identity = info_.id + ";" + info_.version;
        info_.version = std::string(descriptor->plugin_version)
            + ";a=" + std::to_string(private_decoder_plugin_abi_version)
            + ";c=" + compact_identity(std::to_string(
                private_decoder_plugin_interface_contract_token
            ))
            + ";p="
            + std::to_string(descriptor->raw_development_plan_schema_version)
            + ";f="
            + std::to_string(descriptor->raw_frame_schema_version)
            + ";w=" + compact_identity(wrapped_identity);
        info_.version += ";m=" + std::move(module_binary_identity);
        if (info_.version.size() > 128U) {
            throw_plugin_error("private decoder provider cache identity exceeds 128 bytes");
        }
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

void validate_private_decoder_plugin_interface_contract(const std::uint64_t token) {
    if (token != private_decoder_plugin_interface_contract_token) {
        throw_plugin_error(
            "private decoder plugin interface contract is stale; rebuild and replace the local module"
        );
    }
}

void validate_private_decoder_plugin_descriptor(
    const PrivateDecoderPluginDescriptor& descriptor
) {
    if (descriptor.abi_version != private_decoder_plugin_abi_version) {
        throw_plugin_error("private decoder plugin ABI version is unsupported");
    }
    if (descriptor.raw_development_plan_schema_version != raw_development_plan_schema_version) {
        throw_plugin_error("private decoder plugin RAW development plan schema is unsupported");
    }
    if (descriptor.raw_frame_schema_version != raw_frame_schema_version) {
        throw_plugin_error("private decoder plugin RAW frame schema is unsupported");
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
    return std::make_unique<PluginDecoderProvider>(
        std::move(module),
        provider,
        private_module_binary_identity(module_path)
    );
}

} // namespace shadow::image
