#include <shadow/image/decoder_error.hpp>

#include <utility>

namespace shadow::image {

DecodeError::DecodeError(
    const DecodeErrorCode code,
    const int provider_code,
    std::string message
)
    : std::runtime_error(std::move(message)),
      code_(code),
      provider_code_(provider_code) {}

DecodeErrorCode DecodeError::code() const noexcept {
    return code_;
}

int DecodeError::provider_code() const noexcept {
    return provider_code_;
}

} // namespace shadow::image
