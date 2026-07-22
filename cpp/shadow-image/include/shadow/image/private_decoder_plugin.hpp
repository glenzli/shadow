#pragma once

#include <shadow/image/decoder.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace shadow::image {

// Private decoder plugins are an optional local escape hatch for a photographer's legitimately
// installed vendor SDK. The public Shadow repository never ships a vendor SDK, its headers, a
// wrapper library, or camera-specific proprietary decode code. A plugin stays outside the Git
// tree and is loaded only from an explicit local path.
// Version 2 changes the provider-neutral `PixelBuffer` value returned through the C++ decoder
// interfaces by adding RAW-development provenance.  This is an intentional fail-closed bump:
// a v1 private module must be rebuilt before a v2 host is allowed to construct its provider.
// The bridge is still deliberately local/private, but accepting an object whose C++ layout no
// longer agrees with the host would be memory-unsafe rather than merely feature-incomplete.
inline constexpr std::uint32_t private_decoder_plugin_abi_version = 2U;

// This C-compatible descriptor is the discovery contract. The provider factory below deliberately
// crosses a versioned *local C++* ABI, not a stable public C ABI; a private module must be built
// with the matching Shadow headers/toolchain whenever this ABI version changes. The descriptor's
// returned strings remain owned by the plugin and must be static for its lifetime.
struct PrivateDecoderPluginDescriptor final {
    std::uint32_t abi_version = 0U;
    const char* plugin_id = nullptr;
    const char* plugin_version = nullptr;
};

extern "C" {
using PrivateDecoderPluginDescriptorFn = const PrivateDecoderPluginDescriptor* (*)();
using CreatePrivateDecoderProviderFn = DecoderProvider* (*)();
using DestroyPrivateDecoderProviderFn = void (*)(DecoderProvider*);
}

// A private plugin exports these three exact symbols. `create` and `destroy` are paired so the
// private module remains responsible for any allocator/runtime used by its SDK wrapper.
inline constexpr const char* private_decoder_plugin_descriptor_symbol =
    "shadow_private_decoder_plugin_descriptor_v2";
inline constexpr const char* private_decoder_plugin_create_symbol =
    "shadow_create_private_decoder_provider_v2";
inline constexpr const char* private_decoder_plugin_destroy_symbol =
    "shadow_destroy_private_decoder_provider_v2";

// Validates the stable part of the ABI before module construction. The dynamic loader calls this
// too; exposing it makes a private repository able to test its artifact without copying host
// validation logic.
void validate_private_decoder_plugin_descriptor(const PrivateDecoderPluginDescriptor& descriptor);

// Loads one explicitly selected private module. It is intentionally not a directory scanner,
// auto-updater, or bundled fallback. If no local path is configured, LibRaw remains the normal
// public provider. The returned adapter keeps the shared library loaded until every provider
// session has been destroyed.
[[nodiscard]] std::unique_ptr<DecoderProvider> load_private_decoder_plugin(
    const std::filesystem::path& module_path
);

} // namespace shadow::image
