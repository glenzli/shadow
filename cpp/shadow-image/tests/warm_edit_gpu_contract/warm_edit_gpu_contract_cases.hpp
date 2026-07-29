#pragma once

namespace shadow::image::warm_edit_gpu_contract {

[[nodiscard]] int run_resident_backend_matches_cpu_oracle();
[[nodiscard]] int run_resident_gpu_technical_detail_contract();
[[nodiscard]] int run_resident_gpu_texture_contract();
[[nodiscard]] int run_resident_gpu_clarity_contract();
[[nodiscard]] int run_resident_gpu_local_contrast_contract();
[[nodiscard]] int run_resident_gpu_selective_tone_contract();
[[nodiscard]] int run_resident_gpu_composed_stage_contract();
[[nodiscard]] int run_resident_gpu_layer_composition_contract();
[[nodiscard]] int run_resident_gpu_dehaze_and_defringe_contract();
[[nodiscard]] int run_advanced_resource_cache_contract();
[[nodiscard]] int run_perceptual_resource_cache_contract();
[[nodiscard]] int run_color_warper_resource_cache_contract();
[[nodiscard]] int run_cancellation_contract();
[[nodiscard]] int run_benchmark_when_requested();

} // namespace shadow::image::warm_edit_gpu_contract
