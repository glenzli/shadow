#include "detail_tile_contract_test_support.hpp"

#include <shadow/image/full_edit_detail.hpp>

#include <shadow/image/edit_error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using shadow::image::test_support::SyntheticDecodeSession;
using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::neutral_plan;
using shadow::image::test_support::reference_rgb;

using shadow::image::test_support::expect_decode_error;

void neighborhood_resource_limits_fail_closed_before_allocation() {
    constexpr image::Dimensions dimensions{32, 32};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    std::vector<image::AdjustmentNode> plan;
    plan.reserve(35U);
    for (std::size_t index = 0U; index < 35U; ++index) {
        plan.push_back(image::AdjustmentNode{
            .node_id = "apron-limit-sharpen-" + std::to_string(index),
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::technical_detail_implementation_version,
            .parameters = image::SharpenAdjustment{.amount = 1.0, .radius = 5.0},
        });
    }
    expect_decode_error(
        [&] { static_cast<void>(session.render_rgb8(plan, {0, 0, 1, 1})); },
        image::DecodeErrorCode::resource_limit,
        "a spatial plan exceeding the 512-pixel cumulative apron fails closed"
    );
}

void tile_shape_bounds_and_plan_fail_closed() {
    constexpr image::Dimensions dimensions{4, 3};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const auto plan = neutral_plan();
    for (const image::DetailTileRect rect : std::array{
             image::DetailTileRect{0, 0, 0, 1},
             image::DetailTileRect{0, 0, 1, 0},
             image::DetailTileRect{0, 0, image::maximum_edit_detail_tile_side + 1U, 1},
             image::DetailTileRect{4, 0, 1, 1},
             image::DetailTileRect{3, 0, 2, 1},
             image::DetailTileRect{0, 2, 1, 2},
             image::DetailTileRect{std::numeric_limits<std::uint32_t>::max(), 0, 1, 1},
         }) {
        expect_decode_error(
            [&] { static_cast<void>(session.render_rgb8(plan, rect)); },
            image::DecodeErrorCode::invalid_request,
            "invalid detail rectangle is rejected without unsigned overflow"
        );
    }

    const std::array invalid_plan{
        image::AdjustmentNode{
            .node_id = "invalid-exposure",
            .parameters = image::ExposureAdjustment{
                .stops = std::numeric_limits<double>::quiet_NaN(),
            },
        },
    };
    try {
        static_cast<void>(session.render_rgb8(invalid_plan, {0, 0, 0, 0}));
        expect(false, "invalid plan must fail before the invalid rectangle is evaluated");
    } catch (const image::EditError& error) {
        expect(
            error.code() == image::EditErrorCode::invalid_parameter,
            "detail render prevalidates the complete adjustment plan"
        );
    }
}

void metadata_limit_fails_before_reference_render() {
    constexpr image::Dimensions oversized{10'000, 10'000};
    constexpr image::Dimensions tiny{1, 1};
    SyntheticDecodeSession decoder(metadata(oversized), reference_rgb(tiny));
    expect_decode_error(
        [&] { static_cast<void>(image::prepare_full_edit_detail(decoder)); },
        image::DecodeErrorCode::resource_limit,
        "oversized metadata is rejected by the 512 MiB RGB u16 preflight"
    );
    expect(decoder.render_count() == 0, "metadata preflight runs before reference pixel I/O");
}

} // namespace

int main() {
    neighborhood_resource_limits_fail_closed_before_allocation();
    tile_shape_bounds_and_plan_fail_closed();
    metadata_limit_fails_before_reference_render();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
