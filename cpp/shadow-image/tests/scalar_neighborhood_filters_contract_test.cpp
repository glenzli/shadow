#include "scalar_neighborhood_filters.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace filters = shadow::image::detail;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void reflect101_coordinates_preserve_the_non_repeated_edge_contract() {
    expect(filters::reflect101_index(-2, 4U) == 2U && filters::reflect101_index(-1, 4U) == 1U &&
               filters::reflect101_index(4, 4U) == 2U && filters::reflect101_index(5, 4U) == 1U,
           "reflect-101 mirrors around, rather than repeats, each edge sample");
    expect(filters::reflect101_index(-7, 1U) == 0U && filters::reflect101_index(9, 1U) == 0U,
           "a one-sample field always resolves to its only coordinate");
}

void gaussian_filter_is_normalized_and_keeps_constant_fields_constant() {
    const auto kernel = filters::gaussian_kernel(1.25, 4U);
    const double sum = std::accumulate(kernel.begin(), kernel.end(), 0.0);
    expect(std::abs(sum - 1.0) < 1.0e-12, "a Gaussian convolution kernel has unit gain");
    expect(std::equal(kernel.begin(), kernel.begin() + kernel.size() / 2U, kernel.rbegin()),
           "a Gaussian convolution kernel remains symmetric");

    constexpr std::size_t width = 7U;
    constexpr std::size_t height = 5U;
    const std::vector<double> constant(width * height, 0.375);
    const auto blurred = filters::gaussian_blur_scalar(constant, width, height, 0.8, 1.7);
    expect(std::ranges::all_of(
               blurred, [](const double value) { return std::abs(value - 0.375) < 1.0e-12; }),
           "reflect-101 Gaussian filtering leaves a constant scalar field unchanged");
}

void prepared_guided_filter_keeps_shape_radius_and_statistics_together() {
    constexpr std::size_t width = 6U;
    constexpr std::size_t height = 1U;
    constexpr std::uint32_t radius = 1U;
    const std::vector<float> guide{0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F};
    const auto prepared = filters::prepare_replicated_guided_filter(guide, width, height, radius);
    expect(prepared.width() == width && prepared.height() == height && prepared.radius() == radius,
           "prepared guided-filter statistics retain their exact raster and radius");

    const auto self = filters::apply_guided_self_filter(guide, prepared, 1.0e-4);
    expect(self.size() == guide.size() && self[3] - self[2] > 0.95F,
           "self-guidance preserves a hard scalar-field boundary");

    const std::vector<float> target{0.1F, 0.1F, 0.1F, 0.9F, 0.9F, 0.9F};
    const auto guided = filters::apply_guided_target_filter(guide, prepared, target, 1.0e-4);
    expect(guided[3] - guided[2] > 0.70F,
           "target guidance preserves a matching edge instead of averaging across it");

    bool rejected_mismatched_field = false;
    try {
        static_cast<void>(
            filters::apply_guided_self_filter(std::vector<float>{0.0F}, prepared, 1.0e-4));
    } catch (const std::invalid_argument&) {
        rejected_mismatched_field = true;
    }
    expect(rejected_mismatched_field,
           "prepared guided-filter statistics reject a different raster shape");
}

} // namespace

int main() {
    reflect101_coordinates_preserve_the_non_repeated_edge_contract();
    gaussian_filter_is_normalized_and_keeps_constant_fields_constant();
    prepared_guided_filter_keeps_shape_radius_and_statistics_together();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
