#include <shadow/image/private_decoder_plugin.hpp>

#include <cstdlib>

namespace shadow::image {

namespace {

constexpr PrivateDecoderPluginDescriptor descriptor{
    .abi_version = private_decoder_plugin_abi_version,
    .raw_development_plan_schema_version = raw_development_plan_schema_version,
    .raw_frame_schema_version = raw_frame_schema_version,
    .plugin_id = "stale-private-provider",
    .plugin_version = "0.0.0-stale",
};

} // namespace

// This deliberately models a module built before the exact interface-contract handshake existed.
// Its factory aborts so the host contract test also proves that rejecting the missing seal occurs
// before any provider object can be constructed.
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
