#include "contract_test_assertions.hpp"
#include "fused_raw_contract_test_support.hpp"

#include <shadow/image/fused_raw_development.hpp>

#include <shadow/image/decoder_error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

void invalid_inputs_fail_closed() {
    const image::RawFrameLinearTransform identity{{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    }};
    auto unsupported_orientation = synthetic_frame(1);
    try {
        static_cast<void>(image::develop_bayer_linear_srgb_f32_fused(
            unsupported_orientation,
            identity
        ));
        expect(false, "unsupported orientation is rejected");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported_layout,
            "unsupported orientation returns a typed layout error"
        );
    }

    try {
        static_cast<void>(image::develop_bayer_linear_srgb_f32_fused(
            synthetic_frame(0),
            image::RawFrameLinearTransform{},
            0U
        ));
        expect(false, "invalid transform and zero preview edge are rejected");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "invalid fused request returns a typed request error"
        );
    }

    try {
        static_cast<void>(image::develop_bayer_linear_srgb_f32_fused_with_backend(
            synthetic_frame(0),
            identity,
            std::nullopt,
            image::RawDevelopmentBackendMode::cpu,
            image::RawHighlightRecoveryIntent::conservative
        ));
        expect(false, "unimplemented highlight reconstruction is rejected");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported,
            "unimplemented highlight reconstruction fails with a typed unsupported error"
        );
    }

    for (const image::Dimensions dimensions : {
             image::Dimensions{1U, 4U},
             image::Dimensions{4U, 1U},
         }) {
        auto degenerate = synthetic_frame(0);
        degenerate.descriptor.storage_dimensions = dimensions;
        degenerate.descriptor.active_dimensions = dimensions;
        degenerate.descriptor.active_margins = {};
        degenerate.samples.resize(
            static_cast<std::size_t>(dimensions.width) * dimensions.height
        );
        for (const auto backend : {
                 image::RawDevelopmentBackendMode::cpu,
                 image::RawDevelopmentBackendMode::metal,
             }) {
            try {
                static_cast<void>(image::develop_bayer_linear_srgb_f32_fused_with_backend(
                    degenerate,
                    identity,
                    std::nullopt,
                    backend
                ));
                expect(false, "degenerate Bayer storage is rejected before backend selection");
            } catch (const image::DecodeError& error) {
                expect(
                    error.code() == image::DecodeErrorCode::unsupported_layout,
                    "CPU and Metal reject degenerate Bayer storage with the same typed error"
                );
            }
        }
    }
}

} // namespace

int main() {
    invalid_inputs_fail_closed();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
