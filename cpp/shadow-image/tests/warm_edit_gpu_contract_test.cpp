#include "warm_edit_gpu_contract/warm_edit_gpu_contract_cases.hpp"
#include "scoped_environment.hpp"

#include <cstdlib>

int main() {
    namespace contract = shadow::image::warm_edit_gpu_contract;
    // Neighborhoods and native presentation in this suite are Metal contracts.
    // CUDA's supported stages and complete CPU replay have their own runnable
    // cuda_image_execution_contract_test; another GPU must not impersonate Metal.
    const shadow::image::test_support::ScopedEnvironment metal(
        "SHADOW_IMAGE_ACCELERATION", "metal"
    );

    if (
        std::getenv(
            "SHADOW_TEST_WARM_METAL_FORCE_PRESENTATION_PIPELINE_FAILURE"
        )
        != nullptr
    ) {
        return contract::run_presentation_pipeline_failure_contract() == 0
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    int failures = 0;
    failures += contract::run_resident_backend_matches_cpu_oracle();
    failures += contract::run_resident_gpu_technical_detail_contract();
    failures += contract::run_resident_gpu_texture_contract();
    failures += contract::run_resident_gpu_clarity_contract();
    failures += contract::run_resident_gpu_local_contrast_contract();
    failures += contract::run_resident_gpu_selective_tone_contract();
    failures += contract::run_resident_gpu_composed_stage_contract();
    failures += contract::run_resident_gpu_layer_composition_contract();
    failures += contract::run_resident_gpu_mask_coverage_contract();
    failures += contract::run_resident_gpu_retouch_contract();
    failures += contract::run_resident_gpu_geometry_contract();
    failures += contract::run_resident_gpu_dehaze_and_defringe_contract();
    failures += contract::run_advanced_resource_cache_contract();
    failures += contract::run_perceptual_resource_cache_contract();
    failures += contract::run_color_warper_resource_cache_contract();
    failures += contract::run_cancellation_contract();
    failures += contract::run_benchmark_when_requested();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
