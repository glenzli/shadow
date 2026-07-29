#include "warm_edit_gpu_contract/warm_edit_gpu_contract_cases.hpp"

#include <cstdlib>

int main() {
    namespace contract = shadow::image::warm_edit_gpu_contract;

    int failures = 0;
    failures += contract::run_resident_backend_matches_cpu_oracle();
    failures += contract::run_resident_gpu_technical_detail_contract();
    failures += contract::run_resident_gpu_texture_contract();
    failures += contract::run_resident_gpu_clarity_contract();
    failures += contract::run_resident_gpu_local_contrast_contract();
    failures += contract::run_resident_gpu_selective_tone_contract();
    failures += contract::run_resident_gpu_composed_stage_contract();
    failures += contract::run_resident_gpu_layer_composition_contract();
    failures += contract::run_resident_gpu_dehaze_and_defringe_contract();
    failures += contract::run_advanced_resource_cache_contract();
    failures += contract::run_perceptual_resource_cache_contract();
    failures += contract::run_color_warper_resource_cache_contract();
    failures += contract::run_cancellation_contract();
    failures += contract::run_benchmark_when_requested();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
