#include "contract_test_assertions.hpp"

#include <shadow/image/decoder_error.hpp>

#include <array>
#include <cstdlib>
#include <string>

namespace image = shadow::image;

int main() {
    using shadow::image::test_support::expect;
    using shadow::image::test_support::failures;

    constexpr std::array codes{
        image::DecodeErrorCode::unsupported,
        image::DecodeErrorCode::io,
        image::DecodeErrorCode::corrupt_data,
        image::DecodeErrorCode::no_preview,
        image::DecodeErrorCode::unsupported_layout,
        image::DecodeErrorCode::invalid_request,
        image::DecodeErrorCode::resource_limit,
        image::DecodeErrorCode::cancelled,
        image::DecodeErrorCode::internal,
    };
    for (const auto code : codes) {
        const image::DecodeError error(code, -73, "provider diagnostic");
        expect(
            error.code() == code,
            "DecodeError retains its stable category"
        );
        expect(
            error.provider_code() == -73,
            "DecodeError retains the provider code verbatim"
        );
        expect(
            std::string(error.what()) == "provider diagnostic",
            "DecodeError retains the diagnostic through runtime_error"
        );
    }

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
