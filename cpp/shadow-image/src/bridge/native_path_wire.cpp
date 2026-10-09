#include "native_path_wire.hpp"

#include <shadow/image/decoder_error.hpp>

#include <string>
#include <string_view>

namespace shadow::bridge::native_path_wire {

namespace {

void reject_embedded_nul(const std::string_view bytes) {
    if (bytes.find('\0') != std::string_view::npos) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "decoder path contains an embedded NUL"
        );
    }
}

} // namespace

std::filesystem::path from_utf8(const rust::Str path) {
    const std::string_view bytes(path.data(), path.size());
    reject_embedded_nul(bytes);
    std::u8string encoded;
    encoded.reserve(bytes.size());
    for (const char byte : bytes) {
        encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path(encoded);
}

std::filesystem::path from_unix_bytes(const rust::Slice<const std::uint8_t> path) {
#if defined(_WIN32)
    static_cast<void>(path);
    throw image::DecodeError(
        image::DecodeErrorCode::invalid_request,
        0,
        "Unix native path bytes are unavailable on this platform"
    );
#else
    const std::string_view bytes(reinterpret_cast<const char*>(path.data()), path.size());
    reject_embedded_nul(bytes);
    return std::filesystem::path(std::string(bytes));
#endif
}

} // namespace shadow::bridge::native_path_wire
