#include <shadow/image/private_decoder_plugin.hpp>

namespace shadow::image {

namespace {

constexpr PrivateDecoderPluginDescriptor descriptor{
    .abi_version = private_decoder_plugin_abi_version,
    .raw_development_plan_schema_version = raw_development_plan_schema_version,
    .raw_frame_schema_version = raw_frame_schema_version,
    .plugin_id = "libraw-dummy",
    .plugin_version = "0.1.0-dev",
};

} // namespace

// This module is deliberately mundane: it is a fully dynamic private-provider artifact whose
// implementation delegates straight back to the public LibRaw provider. It contains no vendor
// SDK code or camera-specific decoder. Its purpose is to exercise Shadow's exact local-module
// lifecycle, router precedence, RawFrame contract and cache identity before a user adds a truly
// private provider to the same boundary.
extern "C" std::uint64_t shadow_private_decoder_plugin_interface_contract_20260822_1() {
    return private_decoder_plugin_interface_contract_token;
}

extern "C" const PrivateDecoderPluginDescriptor*
shadow_private_decoder_plugin_descriptor_20260822_1() {
    return &descriptor;
}

extern "C" DecoderProvider* shadow_create_private_decoder_provider_20260822_1() {
    return make_libraw_decoder_provider().release();
}

extern "C" void shadow_destroy_private_decoder_provider_20260822_1(DecoderProvider* provider) {
    delete provider;
}

} // namespace shadow::image
