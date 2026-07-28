#include "contract_test_assertions.hpp"
#include "fused_raw_contract_test_support.hpp"

#include <shadow/image/fused_raw_development.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

[[nodiscard]] bool environment_enabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

void metal_full_resolution_stays_within_the_linear_u16_contract() {
    expect(
        image::raw_development_backend_identity(image::RawDevelopmentBackend::cpu)
            != image::raw_development_backend_identity(image::RawDevelopmentBackend::metal),
        "CPU and Metal development identities remain distinct"
    );
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        expect(
            !environment_enabled("SHADOW_TEST_REQUIRE_METAL"),
            "Metal was required for this validation run but no Metal backend is available"
        );
        return;
    }
    const image::RawFrameLinearTransform transform{{
        1.31, -0.27, 0.08,
        -0.06, 1.14, -0.03,
        0.04, -0.22, 1.57,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        const auto frame = synthetic_frame(orientation);
        const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::cpu
        );
        const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal
        );
        const auto repeated = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::metal
        );
        expect(metal.valid(), "Metal full result has a complete typed contract");
        expect(
            metal.backend == image::RawDevelopmentBackend::metal,
            "forced Metal result records its effective backend"
        );
        expect(
            metal.scene_linear.dimensions == cpu.scene_linear.dimensions
                && metal.scene_linear.samples.size() == cpu.scene_linear.samples.size(),
            "Metal preserves CPU dimensions and packed sample count"
        );
        expect(
            metal.scene_linear.samples == repeated.scene_linear.samples,
            "repeated Metal development is byte deterministic"
        );

        std::vector<float> differences;
        differences.reserve(cpu.scene_linear.samples.size());
        double total_difference = 0.0;
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); ++index) {
            const float difference = std::abs(
                cpu.scene_linear.samples[index] - metal.scene_linear.samples[index]
            );
            differences.push_back(difference);
            total_difference += difference;
        }
        std::sort(differences.begin(), differences.end());
        const auto p99_index = differences.empty()
            ? 0U : (differences.size() - 1U) * 99U / 100U;
        const auto maximum = differences.empty() ? 0U : differences.back();
        const auto p99 = differences.empty() ? 0U : differences[p99_index];
        const double mean = differences.empty()
            ? 0.0
            : static_cast<double>(total_difference)
                / static_cast<double>(differences.size());
        expect(maximum <= 4.0e-5F, "Metal maximum error stays within fp32 reconstruction tolerance");
        expect(p99 <= 2.0e-5F, "Metal p99 error stays within fp32 reconstruction tolerance");
        expect(mean <= 1.0e-6, "Metal mean error stays within fp32 reconstruction tolerance");
    }
}

void metal_area_preview_preserves_the_cfa_footprint_contract() {
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        expect(
            !environment_enabled("SHADOW_TEST_REQUIRE_METAL"),
            "Metal was required for area-preview validation but no Metal backend is available"
        );
        return;
    }
    const image::RawFrameLinearTransform transform{{
        1.31, -0.27, 0.08,
        -0.06, 1.14, -0.03,
        0.04, -0.22, 1.57,
    }};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        const auto frame = synthetic_frame(orientation);
        const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            3U,
            image::RawDevelopmentBackendMode::cpu
        );
        const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            3U,
            image::RawDevelopmentBackendMode::metal
        );
        const auto repeated = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            frame,
            transform,
            3U,
            image::RawDevelopmentBackendMode::metal
        );
        expect(
            metal.valid()
                && metal.backend == image::RawDevelopmentBackend::metal
                && metal.demosaic_receipt.algorithm
                    == image::RawDemosaicAlgorithm::bayer_area_preview_v1,
            "Metal area preview retains the typed CFA-footprint receipt"
        );
        expect(
            metal.scene_linear.dimensions == cpu.scene_linear.dimensions
                && metal.scene_linear.samples.size() == cpu.scene_linear.samples.size(),
            "Metal area preview preserves CPU output dimensions and packing"
        );
        expect(
            metal.scene_linear.samples == repeated.scene_linear.samples,
            "Metal area preview is byte deterministic"
        );
        float maximum_error = 0.0F;
        double total_error = 0.0;
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); ++index) {
            const float error = std::abs(
                cpu.scene_linear.samples[index] - metal.scene_linear.samples[index]
            );
            maximum_error = std::max(maximum_error, error);
            total_error += error;
        }
        const double mean_error = cpu.scene_linear.samples.empty()
            ? 0.0
            : static_cast<double>(total_error) / static_cast<double>(cpu.scene_linear.samples.size());
        expect(maximum_error <= 4.0e-5F, "Metal area preview stays within fp32 CPU tolerance");
        expect(mean_error <= 1.0e-5, "Metal area preview stays within fp32 mean tolerance");
    }
}

} // namespace

int main() {
    metal_full_resolution_stays_within_the_linear_u16_contract();
    metal_area_preview_preserves_the_cfa_footprint_contract();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
