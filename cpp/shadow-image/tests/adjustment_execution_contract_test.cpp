#include "adjustment_execution_contract/adjustment_execution_contract_cases.hpp"

#include <iostream>

int main() {
    namespace contract = shadow::image::adjustment_execution_contract;

    int failures = 0;
    failures += contract::run_neutral_and_disabled_plans_have_no_backend_route();
    failures += contract::run_unsupported_operations_are_whole_stage_fallbacks();
    failures += contract::run_fp32_unsafe_curve_and_lut_domains_fall_back_before_dispatch();
    failures += contract::run_opponent_balance_and_local_contrast_have_explicit_cpu_contract();
    failures += contract::run_malformed_disabled_nodes_fail_before_backend_selection();
    failures += contract::run_backend_availability_and_resource_failure_are_explicit();
    failures += contract::run_advanced_pixel_local_operations_match_the_cpu_oracle();
    failures += contract::run_perceptual_color_matches_cpu_and_display_oracles_on_metal();
    failures += contract::run_every_core_order_matches_the_cpu_oracle();
    failures += contract::run_randomized_and_endpoint_parameters_match_the_cpu_oracle();
    failures += contract::run_repeated_nodes_and_concurrent_renders_are_deterministic();
    failures += contract::run_optional_true_machine_benchmark();
    if (failures != 0) {
        std::cerr << failures << " adjustment execution contract checks failed\n";
        return 1;
    }
    std::cout << "adjustment execution contract checks passed\n";
    return 0;
}
