#pragma once

#include <shadow/image/decoder_session.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace shadow::image {

// Private decoder plugins are an optional local escape hatch for a photographer's legitimately
// installed vendor SDK. The public Shadow repository never ships a vendor SDK, its headers, a
// wrapper library, or camera-specific proprietary decode code. A plugin stays outside the Git
// tree and is loaded only from an explicit local path.
// Shadow is still in fast local iteration, so this dated revision denotes the only current C++ ABI
// rather than a compatibility ladder. A RawFrame or provider-layout change means rebuilding every
// local module and replacing the old artifact in place.
inline constexpr std::uint32_t private_decoder_plugin_abi_version = 2'026'082'201U;

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

namespace private_decoder_plugin_contract_detail {

// Size/alignment changes are the common accidental stale-module failure: a provider built before
// AssetMetadata gained GPS fields, for example, returned an older std::string layout that the host
// then read as the new GPS values. Mix every value byte-wise so host and plugin calculate the same
// scalar C-ABI seal from the headers and toolchain which actually compiled them.
[[nodiscard]] consteval std::uint64_t mix_contract_value(
    std::uint64_t hash,
    std::uint64_t value
) noexcept {
    constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;
    for (std::size_t byte = 0U; byte < sizeof(value); ++byte) {
        hash ^= value & 0xffU;
        hash *= fnv_prime;
        value >>= 8U;
    }
    return hash;
}

template <typename Type>
[[nodiscard]] consteval std::uint64_t mix_contract_layout(std::uint64_t hash) noexcept {
    hash = mix_contract_value(hash, sizeof(Type));
    return mix_contract_value(hash, alignof(Type));
}

// Layout hashing cannot observe virtual-method order or a same-size semantic reinterpretation.
// Rotate this epoch for either of those changes. Ordinary field additions and toolchain/stdlib
// layout changes rotate the final token automatically.
inline constexpr std::uint64_t semantic_epoch = 0x6d4c0f7be2a19583ULL;

[[nodiscard]] consteval std::uint64_t calculate_interface_contract_token() noexcept {
    std::uint64_t hash = semantic_epoch;
    hash = mix_contract_value(hash, private_decoder_plugin_abi_version);
    hash = mix_contract_value(hash, raw_development_plan_schema_version);
    hash = mix_contract_value(hash, raw_frame_schema_version);
    hash = mix_contract_layout<PrivateDecoderPluginDescriptor>(hash);
    hash = mix_contract_layout<DecoderProvider>(hash);
    hash = mix_contract_layout<DecodeSession>(hash);
    hash = mix_contract_layout<ProviderInfo>(hash);
    hash = mix_contract_layout<DecodeCapabilities>(hash);
    hash = mix_contract_layout<AssetMetadata>(hash);
    hash = mix_contract_layout<PreviewDescriptor>(hash);
    hash = mix_contract_layout<PreviewPayload>(hash);
    hash = mix_contract_layout<RawDevelopmentPlan>(hash);
    hash = mix_contract_layout<RawDevelopmentCapabilities>(hash);
    hash = mix_contract_layout<RawDevelopmentPlanNegotiation>(hash);
    hash = mix_contract_layout<RawFrameDescriptor>(hash);
    hash = mix_contract_layout<RawFrame>(hash);
    hash = mix_contract_layout<PixelBuffer>(hash);
    return hash;
}

} // namespace private_decoder_plugin_contract_detail

// This dated ABI revision and seal are the exact build-interface handshake for the one moving
// local ABI. The seal changes automatically when the
// size/alignment of a crossing type changes; virtual-order or same-layout semantic changes rotate
// the epoch above. It is an accidental-stale-binary guard, not a security primitive and not a
// compatibility version. A module with any older seal is simply rebuilt and replaced.
inline constexpr std::uint64_t private_decoder_plugin_interface_contract_token =
    private_decoder_plugin_contract_detail::calculate_interface_contract_token();

extern "C" {
using PrivateDecoderPluginInterfaceContractFn = std::uint64_t (*)();
using PrivateDecoderPluginDescriptorFn = const PrivateDecoderPluginDescriptor* (*)();
using CreatePrivateDecoderProviderFn = DecoderProvider* (*)();
using DestroyPrivateDecoderProviderFn = void (*)(DecoderProvider*);
}

// A private plugin exports these four exact symbols. The scalar interface-contract function is
// deliberately resolved and checked before the host calls the descriptor or provider factory, so
// an older module cannot be mistaken for current merely because both report an ABI revision. `create`
// and `destroy` are paired so the private module remains responsible for any allocator/runtime
// used by its SDK wrapper.
inline constexpr const char* private_decoder_plugin_interface_contract_symbol =
    "shadow_private_decoder_plugin_interface_contract_20260822_1";
inline constexpr const char* private_decoder_plugin_descriptor_symbol =
    "shadow_private_decoder_plugin_descriptor_20260822_1";
inline constexpr const char* private_decoder_plugin_create_symbol =
    "shadow_create_private_decoder_provider_20260822_1";
inline constexpr const char* private_decoder_plugin_destroy_symbol =
    "shadow_destroy_private_decoder_provider_20260822_1";

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
