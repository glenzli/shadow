#include "adjustment_execution_contract_cases.hpp"
#include "execution_parity_fixture.hpp"

#include "../scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_error.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace image = shadow::image;

namespace shadow::image::adjustment_execution_contract {

namespace {

using shadow::image::test_support::ScopedEnvironment;
using parity_fixture::close_to_cpu;
using parity_fixture::make_image;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void backend_availability_and_resource_failure_are_explicit() {
    const auto input = make_image(17U, 11U);
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "active-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.25},
        },
    };
    const auto cpu = image::execute_adjustment_nodes(input, nodes);
    if (!image::adjustment_backend_available(image::AdjustmentBackend::metal)) {
        const bool metal_required =
            std::getenv("SHADOW_TEST_REQUIRE_METAL") != nullptr;
        const auto automatic = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::automatic
        );
        expect(
            automatic.backend == image::AdjustmentBackend::cpu
                && automatic.fell_back && automatic.pixels.samples == cpu.samples,
            "CPU-only automatic selection replays the complete stage"
        );
        try {
            static_cast<void>(image::execute_adjustment_nodes_with_backend(
                input,
                nodes,
                {},
                image::AdjustmentBackendMode::metal
            ));
            expect(false, "forced Metal fails when its runtime is unavailable");
        } catch (const image::EditError& error) {
            if (metal_required) {
                std::cerr << "Metal adjustment diagnostic: " << error.what() << '\n';
            }
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "unavailable forced Metal returns a typed backend failure"
            );
        }
        expect(!metal_required, "Metal was required but its adjustment backend is unavailable");
        return;
    }

    {
        const ScopedEnvironment tiny_budget(
            "SHADOW_TEST_METAL_ADJUSTMENT_TILE_BYTES",
            "1"
        );
        const auto automatic = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::automatic
        );
        expect(
            automatic.backend == image::AdjustmentBackend::cpu
                && automatic.fell_back && automatic.pixels.samples == cpu.samples,
            "resource rejection replays the complete adjustment stage on CPU"
        );
        try {
            static_cast<void>(image::execute_adjustment_nodes_with_backend(
                input,
                nodes,
                {},
                image::AdjustmentBackendMode::metal
            ));
            expect(false, "forced Metal exposes resource rejection");
        } catch (const image::EditError& error) {
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "forced resource rejection remains a backend failure"
            );
        }
    }
    {
        // 17 RGB float pixels require 204 bytes per row and 408 bytes for separate input/output.
        // A 900-byte budget admits two rows, forcing this 11-row image through six GPU tiles.
        const ScopedEnvironment multi_tile_budget(
            "SHADOW_TEST_METAL_ADJUSTMENT_TILE_BYTES",
            "900"
        );
        const auto metal = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::metal
        );
        double error = 0.0;
        expect(
            metal.backend == image::AdjustmentBackend::metal
                && !metal.fell_back
                && close_to_cpu(metal.pixels, cpu, error),
            "successful bounded multi-tile Metal execution matches CPU"
        );
    }
    {
        const ScopedEnvironment injected(
            "SHADOW_TEST_METAL_ADJUSTMENT_FORCE_FAILURE",
            "1"
        );
        const auto automatic = image::execute_adjustment_nodes_with_backend(
            input,
            nodes,
            {},
            image::AdjustmentBackendMode::automatic
        );
        expect(
            automatic.backend == image::AdjustmentBackend::cpu
                && automatic.fell_back && automatic.pixels.samples == cpu.samples,
            "runtime GPU failure discards all GPU output and replays CPU"
        );
        try {
            static_cast<void>(image::execute_adjustment_nodes_with_backend(
                input,
                nodes,
                {},
                image::AdjustmentBackendMode::metal
            ));
            expect(false, "forced Metal exposes an injected runtime failure");
        } catch (const image::EditError& error) {
            expect(
                error.code() == image::EditErrorCode::backend_failure,
                "injected forced-Metal failure remains typed"
            );
        }
    }
}
} // namespace

int run_backend_availability_and_resource_failure_are_explicit() {
    failures = 0;
    backend_availability_and_resource_failure_are_explicit();
    return failures;
}

} // namespace shadow::image::adjustment_execution_contract
