#include <shadow/image/private_decoder_plugin.hpp>

#include <cstdlib>

namespace shadow::image {

namespace {

// This is the exact seal used by modules built before AssetMetadata gained its GPS fields. The
// fixture otherwise compiles against current headers so the loader test proves that a scalar
// contract mismatch is rejected before it can call any stale C++ descriptor or factory.
constexpr std::uint64_t pre_gps_interface_contract_token = 0x8e5f4b2ad30c71a9ULL;

constexpr PrivateDecoderPluginDescriptor descriptor{
    .abi_version = private_decoder_plugin_abi_version,
    .raw_development_plan_schema_version = raw_development_plan_schema_version,
    .raw_frame_schema_version = raw_frame_schema_version,
    .plugin_id = "stale-private-provider",
    .plugin_version = "0.0.0-stale",
};

} // namespace

extern "C" std::uint64_t shadow_private_decoder_plugin_interface_contract_v1() {
    return pre_gps_interface_contract_token;
}

// The factory aborts so the host contract test proves the mismatched scalar seal is rejected before
// any provider object can be constructed or any stale C++ layout is dereferenced.
extern "C" const PrivateDecoderPluginDescriptor*
shadow_private_decoder_plugin_descriptor_v1() {
    return &descriptor;
}

extern "C" DecoderProvider* shadow_create_private_decoder_provider_v1() {
    std::abort();
}

extern "C" void shadow_destroy_private_decoder_provider_v1(DecoderProvider*) {
    std::abort();
}

} // namespace shadow::image
