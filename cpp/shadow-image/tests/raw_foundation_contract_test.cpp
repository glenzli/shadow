#include <shadow/image/decoder_error.hpp>
#include <shadow/image/raw_foundation.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using shadow::image::DecodeError;
using shadow::image::Dimensions;
using shadow::image::RawCfaColor;
using shadow::image::RawFoundationCameraRgbView;
using shadow::image::RawFoundationProvenance;
using shadow::image::RawFrameCfaLayout;
using shadow::image::RawFrameDescriptor;
using shadow::image::RawFrameLinearTransform;

constexpr const char* digest_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* digest_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* digest_c = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "raw foundation contract failure: " << message << '\n';
    std::exit(1);
}

void require(const bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

RawFrameDescriptor descriptor(const Dimensions active, const std::int32_t orientation = 0) {
    RawFrameDescriptor descriptor;
    descriptor.active_dimensions = active;
    descriptor.storage_dimensions = active;
    descriptor.orientation = orientation;
    descriptor.cfa_layout = RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        RawCfaColor::red,
        RawCfaColor::green,
        RawCfaColor::green,
        RawCfaColor::blue,
    };
    return descriptor;
}

RawFoundationProvenance provenance() {
    return {
        .source_sha256 = digest_a,
        .artifact_file_sha256 = digest_b,
        .cache_key_sha256 = digest_c,
        .model_identity = std::string(shadow::image::raw_foundation_model_identity),
        .implementation_revision =
            std::string(shadow::image::raw_foundation_implementation_revision),
    };
}

void full_resolution_transform_and_orientation_are_exact() {
    const std::vector<float> pixels{
        1.0F,
        2.0F,
        3.0F,
        4.0F,
        5.0F,
        6.0F,
        7.0F,
        8.0F,
        9.0F,
        10.0F,
        11.0F,
        12.0F,
    };
    const RawFoundationCameraRgbView input{
        .dimensions = {.width = 2U, .height = 2U},
        .samples = pixels,
        .provenance = provenance(),
    };
    const RawFrameLinearTransform transform{{
        2.0,
        0.0,
        0.0,
        0.0,
        3.0,
        0.0,
        0.0,
        0.0,
        4.0,
    }};
    const auto developed = shadow::image::develop_raw_foundation(
        input,
        descriptor({.width = 2U, .height = 2U}, 6),
        transform
    );
    require(developed.valid(), "full-resolution result must be valid");
    require(
        developed.scene_linear.dimensions == Dimensions{.width = 2U, .height = 2U},
        "square orientation retains dimensions"
    );
    const std::vector<float> expected{
        14.0F,
        24.0F,
        36.0F,
        2.0F,
        6.0F,
        12.0F,
        20.0F,
        33.0F,
        48.0F,
        8.0F,
        15.0F,
        24.0F,
    };
    require(developed.scene_linear.samples == expected, "orientation 6 and matrix must be exact");
    require(!developed.bounded_preview, "full-resolution conversion is not a preview");
    require(
        developed.cache_identity.find(std::string(digest_b)) != std::string::npos,
        "cache identity contains the verified artifact digest"
    );
}

void bounded_preview_is_bilinear_before_the_linear_transform() {
    std::vector<float> pixels;
    pixels.reserve(4U * 4U * 3U);
    for (std::uint32_t y = 0U; y < 4U; ++y) {
        for (std::uint32_t x = 0U; x < 4U; ++x) {
            const float value = static_cast<float>(y * 10U + x);
            pixels.insert(pixels.end(), {value, value + 100.0F, value + 200.0F});
        }
    }
    const RawFoundationCameraRgbView input{
        .dimensions = {.width = 4U, .height = 4U},
        .samples = pixels,
        .provenance = provenance(),
    };
    const RawFrameLinearTransform identity{{
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    }};
    const auto developed = shadow::image::develop_raw_foundation(
        input,
        descriptor({.width = 4U, .height = 4U}),
        identity,
        2U
    );
    require(developed.bounded_preview, "smaller max edge selects bounded preview");
    require(
        developed.scene_linear.dimensions == Dimensions{.width = 2U, .height = 2U},
        "bounded preview preserves aspect ratio"
    );
    const std::vector<float> expected{
        5.5F,
        105.5F,
        205.5F,
        7.5F,
        107.5F,
        207.5F,
        25.5F,
        125.5F,
        225.5F,
        27.5F,
        127.5F,
        227.5F,
    };
    require(
        developed.scene_linear.samples == expected,
        "bounded preview uses pixel-center bilinear"
    );
}

void crop_geometry_and_provenance_fail_closed() {
    const std::vector<float> pixels(4U * 4U * 3U, 0.25F);
    RawFoundationCameraRgbView input{
        .dimensions = {.width = 4U, .height = 4U},
        .crop_top = 1U,
        .crop_left = 1U,
        .samples = pixels,
        .provenance = provenance(),
    };
    require(
        input.matches_source(descriptor({.width = 6U, .height = 6U})),
        "one-pixel leading and trailing crops match the source active sensor"
    );
    require(
        !input.matches_source(descriptor({.width = 7U, .height = 6U})),
        "more than one trailing pixel fails closed"
    );
    input.provenance.model_identity = "unknown";
    require(!input.valid(), "unknown model identity is rejected");

    bool rejected = false;
    try {
        static_cast<void>(shadow::image::develop_raw_foundation(
            input,
            descriptor({.width = 6U, .height = 6U}),
            RawFrameLinearTransform{{
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
            }}
        ));
    } catch (const DecodeError&) {
        rejected = true;
    }
    require(rejected, "invalid foundation must throw before pixel work");
}

} // namespace

int main() {
    full_resolution_transform_and_orientation_are_exact();
    bounded_preview_is_bilinear_before_the_linear_transform();
    crop_geometry_and_provenance_fail_closed();
    std::cout << "raw foundation contract passed\n";
    return 0;
}
