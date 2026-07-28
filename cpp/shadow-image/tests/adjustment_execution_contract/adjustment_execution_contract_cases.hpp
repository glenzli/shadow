#pragma once

namespace shadow::image::adjustment_execution_contract {

[[nodiscard]] int run_neutral_and_disabled_plans_have_no_backend_route();
[[nodiscard]] int run_unsupported_operations_are_whole_stage_fallbacks();
[[nodiscard]] int run_fp32_unsafe_curve_and_lut_domains_fall_back_before_dispatch();
[[nodiscard]] int run_opponent_balance_and_local_contrast_have_explicit_cpu_contract();
[[nodiscard]] int run_malformed_disabled_nodes_fail_before_backend_selection();
[[nodiscard]] int run_backend_availability_and_resource_failure_are_explicit();
[[nodiscard]] int run_advanced_pixel_local_operations_match_the_cpu_oracle();
[[nodiscard]] int run_perceptual_color_matches_cpu_and_display_oracles_on_metal();
[[nodiscard]] int run_every_core_order_matches_the_cpu_oracle();
[[nodiscard]] int run_randomized_and_endpoint_parameters_match_the_cpu_oracle();
[[nodiscard]] int run_repeated_nodes_and_concurrent_renders_are_deterministic();
[[nodiscard]] int run_optional_true_machine_benchmark();

} // namespace shadow::image::adjustment_execution_contract
