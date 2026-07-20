#pragma once

#include "rust/cxx.h"

namespace shadow::bridge {
class DecodeHandle;
}

#include "shadow-bridge/src/lib.rs.h"

#include <shadow/image/decoder.hpp>

#include <memory>

namespace shadow::bridge {

class DecodeHandle final {
public:
    DecodeHandle(
        std::unique_ptr<image::DecoderProvider> provider,
        std::unique_ptr<image::DecodeSession> session
    );
    ~DecodeHandle();

    DecodeHandle(const DecodeHandle&) = delete;
    DecodeHandle& operator=(const DecodeHandle&) = delete;

    [[nodiscard]] FfiProviderSnapshot provider() const;
    [[nodiscard]] FfiMetadataSnapshot metadata() const;
    [[nodiscard]] FfiCapabilitySnapshot capabilities() const;
    [[nodiscard]] rust::Vec<FfiPreviewSnapshot> previews() const;
    [[nodiscard]] FfiPreviewPayload decode_best_preview();
    [[nodiscard]] FfiEncodedProxy render_reference_proxy(
        std::uint32_t max_edge,
        std::uint8_t jpeg_quality
    ) const;
    [[nodiscard]] FfiEncodedProxy render_edited_reference_proxy(
        const FfiBasicEditRequest& request
    ) const;

private:
    std::unique_ptr<image::DecoderProvider> provider_;
    std::unique_ptr<image::DecodeSession> session_;
};

[[nodiscard]] std::unique_ptr<DecodeHandle> open_libraw_utf8(rust::Str path);
[[nodiscard]] rust::String libraw_provider_version();

} // namespace shadow::bridge
