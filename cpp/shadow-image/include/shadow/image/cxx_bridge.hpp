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

private:
    std::unique_ptr<image::DecoderProvider> provider_;
    std::unique_ptr<image::DecodeSession> session_;
};

[[nodiscard]] std::unique_ptr<DecodeHandle> open_libraw_utf8(rust::Str path);

} // namespace shadow::bridge
