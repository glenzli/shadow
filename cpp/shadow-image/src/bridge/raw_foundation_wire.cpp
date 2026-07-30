#include "raw_foundation_wire.hpp"

#include <shadow/image/cxx_bridge.hpp>

#include <span>
#include <string>

namespace shadow::bridge::raw_foundation_wire {

namespace {

[[nodiscard]] std::string rust_string(const rust::String& value) {
    return std::string(value.data(), value.size());
}

} // namespace

image::RawFoundationCameraRgbView raw_foundation_view(const FfiRawFoundation& foundation) {
    return image::RawFoundationCameraRgbView{
        .dimensions =
            {
                .width = foundation.width,
                .height = foundation.height,
            },
        .crop_top = foundation.crop_top,
        .crop_left = foundation.crop_left,
        .amount_percent = foundation.amount_percent,
        .samples = std::span<const float>(foundation.samples.data(), foundation.samples.size()),
        .provenance = {
            .source_sha256 = rust_string(foundation.source_sha256),
            .artifact_file_sha256 = rust_string(foundation.artifact_file_sha256),
            .cache_key_sha256 = rust_string(foundation.cache_key_sha256),
            .model_identity = rust_string(foundation.model_identity),
            .implementation_revision = rust_string(foundation.implementation_revision),
        },
    };
}

} // namespace shadow::bridge::raw_foundation_wire
