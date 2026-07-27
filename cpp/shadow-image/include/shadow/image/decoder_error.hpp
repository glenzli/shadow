#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace shadow::image {

/// Stable error categories shared by decoder providers and bounded image
/// preprocessing helpers.
enum class DecodeErrorCode : std::uint8_t {
    unsupported,
    io,
    corrupt_data,
    no_preview,
    unsupported_layout,
    invalid_request,
    resource_limit,
    cancelled,
    internal,
};

class DecodeError final : public std::runtime_error {
public:
    DecodeError(DecodeErrorCode code, int provider_code, std::string message);

    [[nodiscard]] DecodeErrorCode code() const noexcept;
    [[nodiscard]] int provider_code() const noexcept;

private:
    DecodeErrorCode code_;
    int provider_code_;
};

} // namespace shadow::image
