#include "contract_test_assertions.hpp"

#include "local_mask_coverage.hpp"
#include "local_mask_validation.hpp"
#include "managed_raster_mask.hpp"
#include "warm_edit_gpu_mask_plan.hpp"

#include <shadow/image/edit_error.hpp>

#include <cmath>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

using image::test_support::expect;
using image::test_support::failures;

[[nodiscard]] image::ManagedRasterMask
raster(const image::ManagedRasterMaskEncoding encoding, std::vector<std::uint8_t> samples) {
    return image::ManagedRasterMask{
        .raster_dimensions = {.width = 2U, .height = 2U},
        .coordinate_dimensions = {.width = 6'000U, .height = 4'000U},
        .encoding = encoding,
        .samples = std::move(samples),
    };
}

[[nodiscard]] image::ManagedRasterMask raster_5x5(std::vector<std::uint8_t> samples) {
    return image::ManagedRasterMask{
        .raster_dimensions = {.width = 5U, .height = 5U},
        .coordinate_dimensions = {.width = 6'000U, .height = 4'000U},
        .encoding = image::ManagedRasterMaskEncoding::gray8,
        .samples = std::move(samples),
    };
}

void expect_close(const double actual, const double expected, const std::string_view message) {
    expect(std::abs(actual - expected) <= 1.0e-12, message);
}

[[nodiscard]] bool rejects(const image::LocalMask& mask) {
    try {
        image::detail::validate_local_mask(mask);
    } catch (const image::EditError& error) {
        return error.code() == image::EditErrorCode::invalid_parameter;
    }
    return false;
}

void gray8_uses_pixel_center_bilinear_sampling() {
    const image::LocalMask mask{
        .kind = image::LocalMaskKind::managed_raster,
        .managed_raster = raster(image::ManagedRasterMaskEncoding::gray8, {0U, 255U, 128U, 64U}),
    };
    image::detail::validate_local_mask(mask);
    const image::detail::PreparedLocalMaskCoverage prepared{.mask = &mask};
    const image::detail::Vector3 ignored_rgb{};

    expect_close(
        image::detail::local_mask_coverage_at(prepared, 0.25, 0.25, ignored_rgb),
        0.0,
        "the top-left raster pixel center is sampled exactly"
    );
    expect_close(
        image::detail::local_mask_coverage_at(prepared, 0.75, 0.25, ignored_rgb),
        1.0,
        "the top-right raster pixel center is sampled exactly"
    );
    expect_close(
        image::detail::local_mask_coverage_at(prepared, 0.5, 0.5, ignored_rgb),
        (0.0 + 1.0 + 128.0 / 255.0 + 64.0 / 255.0) / 4.0,
        "the normalized image center bilinearly mixes all four samples"
    );

    image::LocalMask inverted = mask;
    inverted.invert = true;
    const image::detail::PreparedLocalMaskCoverage inverted_prepared{.mask = &inverted};
    expect_close(
        image::detail::local_mask_coverage_at(inverted_prepared, 0.75, 0.25, ignored_rgb),
        0.0,
        "managed raster inversion is applied after sampling"
    );
}

void gray16_float_is_portable_little_endian_coverage() {
    const image::ManagedRasterMask mask = raster(
        image::ManagedRasterMaskEncoding::gray16_float,
        {
            0x00U,
            0x00U, // 0.0
            0x00U,
            0x38U, // 0.5
            0x00U,
            0x3CU, // 1.0
            0x00U,
            0x34U, // 0.25
        }
    );
    image::detail::validate_managed_raster_mask(mask);
    expect_close(
        image::detail::sample_managed_raster_mask(mask, 0.5, 0.5),
        0.4375,
        "little-endian binary16 samples share the Gray8 bilinear convention"
    );
}

void malformed_payloads_fail_closed() {
    const auto local = [](image::ManagedRasterMask raster_mask) {
        return image::LocalMask{
            .kind = image::LocalMaskKind::managed_raster,
            .managed_raster = std::move(raster_mask),
        };
    };
    expect(
        rejects(local(raster(image::ManagedRasterMaskEncoding::gray8, {0U, 1U, 2U}))),
        "a truncated Gray8 plane is rejected"
    );
    expect(
        rejects(local(raster(
            image::ManagedRasterMaskEncoding::gray16_float,
            {0x00U, 0x00U, 0x00U, 0x38U, 0x01U, 0x3CU, 0x00U, 0x34U}
        ))),
        "binary16 coverage greater than one is rejected"
    );
    expect(
        rejects(local(raster(
            image::ManagedRasterMaskEncoding::gray16_float,
            {0x00U, 0x80U, 0x00U, 0x38U, 0x00U, 0x3CU, 0x00U, 0x34U}
        ))),
        "negative binary16 coverage is rejected"
    );

    image::ManagedRasterMask missing_extent =
        raster(image::ManagedRasterMaskEncoding::gray8, {0U, 1U, 2U, 3U});
    missing_extent.coordinate_dimensions.width = 0U;
    expect(rejects(local(std::move(missing_extent))), "a zero coordinate extent is rejected");

    expect(
        rejects(image::LocalMask{.kind = image::LocalMaskKind::managed_raster}),
        "a managed raster kind without payload is rejected"
    );
    expect(
        rejects(
            image::LocalMask{
                .kind = image::LocalMaskKind::managed_raster,
                .radius_y = 1.01,
                .managed_raster = raster(image::ManagedRasterMaskEncoding::gray8, {0U, 1U, 2U, 3U}),
            }
        ),
        "managed raster expansion outside the signed unit interval is rejected"
    );
    expect(
        rejects(
            image::LocalMask{
                .kind = image::LocalMaskKind::linear_gradient,
                .x0 = 0.0,
                .y0 = 0.0,
                .x1 = 1.0,
                .y1 = 1.0,
                .managed_raster = raster(image::ManagedRasterMaskEncoding::gray8, {0U, 1U, 2U, 3U}),
            }
        ),
        "legacy mask kinds cannot smuggle an immutable raster payload"
    );
}

void composite_topology_is_bounded_and_non_nested() {
    const auto leaf = [] {
        return image::LocalMask{
            .kind = image::LocalMaskKind::linear_gradient,
            .x0 = 0.0,
            .y0 = 0.0,
            .x1 = 1.0,
            .y1 = 1.0,
        };
    };
    image::LocalMask too_many;
    for (std::size_t index = 0U; index <= image::maximum_composite_local_mask_components;
         ++index) {
        too_many.components.push_back(
            image::LocalMaskComponent{
                .operation = index == 0U ? image::LocalMaskComponentOperation::base
                                         : image::LocalMaskComponentOperation::add,
                .mask = leaf(),
            }
        );
    }
    expect(rejects(too_many), "composite masks reject a ninth leaf before execution");

    image::LocalMask wrong_base;
    wrong_base.components.push_back(
        image::LocalMaskComponent{
            .operation = image::LocalMaskComponentOperation::add,
            .mask = leaf(),
        }
    );
    expect(rejects(wrong_base), "the first composite component must establish Base coverage");

    image::LocalMask nested_leaf;
    nested_leaf.components.push_back(
        image::LocalMaskComponent{
            .operation = image::LocalMaskComponentOperation::base,
            .mask = leaf(),
        }
    );
    image::LocalMask nested;
    nested.components.push_back(
        image::LocalMaskComponent{
            .operation = image::LocalMaskComponentOperation::base,
            .mask = std::move(nested_leaf),
        }
    );
    expect(rejects(nested), "composite masks reject recursive component topology");
}

void refinement_is_recomputed_from_the_immutable_soft_mask() {
    std::vector<std::uint8_t> center(25U, 0U);
    center[12U] = 255U;
    const image::ManagedRasterMask source = raster_5x5(center);

    const image::detail::RefinedManagedRasterMask expanded =
        image::detail::refine_managed_raster_mask(source, 1.0, 0.0);
    expect(expanded.valid(), "expanded managed raster has one complete float plane");
    expect_close(
        image::detail::sample_refined_managed_raster_mask(expanded, 0.3, 0.5),
        1.0,
        "positive expansion grows the center sample by the bounded radius"
    );
    expect_close(
        image::detail::sample_refined_managed_raster_mask(expanded, 0.1, 0.1),
        0.0,
        "positive expansion does not reach beyond its bounded neighborhood"
    );

    std::vector<std::uint8_t> square(25U, 0U);
    for (std::size_t row = 1U; row <= 3U; ++row) {
        for (std::size_t column = 1U; column <= 3U; ++column) {
            square[row * 5U + column] = 255U;
        }
    }
    const image::detail::RefinedManagedRasterMask contracted =
        image::detail::refine_managed_raster_mask(raster_5x5(square), -1.0, 0.0);
    expect_close(
        image::detail::sample_refined_managed_raster_mask(contracted, 0.5, 0.5),
        1.0,
        "negative expansion keeps the interior that survives contraction"
    );
    expect_close(
        image::detail::sample_refined_managed_raster_mask(contracted, 0.3, 0.5),
        0.0,
        "negative expansion removes the contracted boundary"
    );

    const image::detail::RefinedManagedRasterMask feathered =
        image::detail::refine_managed_raster_mask(source, 0.0, 1.0);
    expect(
        std::abs(image::detail::sample_refined_managed_raster_mask(feathered, 0.5, 0.5) - 1.0 / 9.0)
            <= 1.0e-6,
        "feather applies one symmetric box transition from the immutable source"
    );
    expect_close(
        image::detail::sample_managed_raster_mask(source, 0.5, 0.5),
        1.0,
        "refinement never mutates the persisted provider samples"
    );
}

void resident_gpu_declines_without_changing_the_cpu_contract() {
    const image::LocalMask mask{
        .kind = image::LocalMaskKind::managed_raster,
        .managed_raster = raster(image::ManagedRasterMaskEncoding::gray8, {0U, 255U, 128U, 64U}),
    };
    const image::FloatRgbImage source{
        .dimensions = {.width = 2U, .height = 2U},
        .row_stride_bytes = 2U * 3U * sizeof(float),
    };
    const auto preparation =
        image::detail::prepare_warm_gpu_mask_plan(source, mask, source.dimensions, {}, 1.0);
    expect(
        !preparation.plan.has_value() && !preparation.diagnostic.empty(),
        "resident Metal explicitly declines managed rasters so the caller can use CPU"
    );
}

} // namespace

int main() {
    gray8_uses_pixel_center_bilinear_sampling();
    gray16_float_is_portable_little_endian_coverage();
    malformed_payloads_fail_closed();
    composite_topology_is_bounded_and_non_nested();
    refinement_is_recomputed_from_the_immutable_soft_mask();
    resident_gpu_declines_without_changing_the_cpu_contract();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
