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
// Shadow is still in fast local iteration, so `1` denotes the only current C++ ABI rather than a
// compatibility ladder. A RawFrame or provider-layout change means rebuilding every local module
// and replacing the old artifact in place.
inline constexpr std::uint32_t private_decoder_plugin_abi_version = 1U;

// The ABI number intentionally remains v1 throughout pre-release development. This seal is the
// exact build-interface handshake for that one moving ABI: change it whenever DecoderProvider,
// DecodeSession, RawFrame, or another C++ type crossing the module boundary changes layout or
// virtual-method order. It is an accidental-stale-binary guard, not a security primitive and not
// a compatibility version. A module with any older seal is simply rebuilt and replaced.
inline constexpr std::uint64_t private_decoder_plugin_interface_contract_token =
    0x8e5f4b2ad30c71a9ULL;

// This C-compatible descriptor is the discovery contract. The provider factory below deliberately
// crosses a versioned *local C++* ABI, not a stable public C ABI; a private module must be built
// with the matching Shadow headers/toolchain whenever this interface changes. The descriptor's
// returned strings remain owned by the plugin and must be static for its lifetime.
struct PrivateDecoderPluginDescriptor final {
    std::uint32_t abi_version = 0U;
    // The matching value proves that this module was compiled against the plan-aware RAW
    // development contract, rather than merely sharing a coincidental C++ ABI number.
    std::uint32_t raw_development_plan_schema_version = 0U;
    // RawFrame carries unprocessed sensor samples and is consumed by the next demosaic/RAW
    // stages. Require an exact schema match before a private provider's virtual method is used.
    std::uint32_t raw_frame_schema_version = 0U;
    const char* plugin_id = nullptr;
    const char* plugin_version = nullptr;
};

extern "C" {
using PrivateDecoderPluginInterfaceContractFn = std::uint64_t (*)();
using PrivateDecoderPluginDescriptorFn = const PrivateDecoderPluginDescriptor* (*)();
using CreatePrivateDecoderProviderFn = DecoderProvider* (*)();
using DestroyPrivateDecoderProviderFn = void (*)(DecoderProvider*);
}

// A private plugin exports these four exact symbols. The scalar interface-contract function is
// deliberately resolved and checked before the host calls the descriptor or provider factory, so
// an older module cannot be mistaken for current merely because both still say ABI v1. `create`
// and `destroy` are paired so the private module remains responsible for any allocator/runtime
// used by its SDK wrapper.
inline constexpr const char* private_decoder_plugin_interface_contract_symbol =
    "shadow_private_decoder_plugin_interface_contract_v1";
inline constexpr const char* private_decoder_plugin_descriptor_symbol =
    "shadow_private_decoder_plugin_descriptor_v1";
inline constexpr const char* private_decoder_plugin_create_symbol =
    "shadow_create_private_decoder_provider_v1";
inline constexpr const char* private_decoder_plugin_destroy_symbol =
    "shadow_destroy_private_decoder_provider_v1";

// Validates the stable part of the ABI before module construction. The dynamic loader calls this
// too; exposing it makes a private repository able to test its artifact without copying host
// validation logic.
void validate_private_decoder_plugin_interface_contract(std::uint64_t token);
void validate_private_decoder_plugin_descriptor(const PrivateDecoderPluginDescriptor& descriptor);

// Loads one explicitly selected private module. It is intentionally not a directory scanner,
// auto-updater, or bundled fallback. If no local path is configured, LibRaw remains the normal
// public provider. The returned adapter keeps the shared library loaded until every provider
// session has been destroyed.
[[nodiscard]] std::unique_ptr<DecoderProvider> load_private_decoder_plugin(
    const std::filesystem::path& module_path
);

} // namespace shadow::image
