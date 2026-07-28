#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"
#include "metal_adjustment_program.hpp"
#include "perceptual_contrast.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <variant>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void scene_contrast_is_restrained_and_preserves_the_middle_gray_anchor() {
    const auto input = rgb_image(3, {
        0.01F,
        0.01F,
        0.01F,
        0.18F,
        0.18F,
        0.18F,
        1.0F,
        1.0F,
        1.0F,
    });
    const std::array contrast_node{
        image::AdjustmentNode{
            .node_id = "restrained-scene-contrast",
            .parameters = image::ContrastAdjustment{.factor = 2.5, .pivot = 0.18},
        },
    };
    const auto adjusted = image::execute_adjustment_nodes(input, contrast_node);
    expect(
        adjusted.samples[0] > 0.003F && adjusted.samples[0] < input.samples[0],
        "maximum UI contrast deepens shadows without crushing them to black"
    );
    expect_close(
        adjusted.samples[3],
        0.18F,
        "scene contrast keeps its explicit middle-gray pivot stable"
    );
    expect(
        adjusted.samples[6] > input.samples[6] && adjusted.samples[6] < 2.0F,
        "maximum UI contrast expands highlights without an implausible hard shoulder"
    );

    const std::array reduced_contrast_node{
        image::AdjustmentNode{
            .node_id = "reduced-scene-contrast",
            .parameters = image::ContrastAdjustment{.factor = 0.25, .pivot = 0.18},
        },
    };
    const auto reduced = image::execute_adjustment_nodes(input, reduced_contrast_node);
    expect(
        reduced.samples[0] > input.samples[0] && reduced.samples[6] < input.samples[6],
        "negative contrast converges toward the same middle-gray pivot"
    );
}

void neutral_and_zero_factor_contracts_are_explicit() {
    const auto extended =
        rgb_image(2, {-0.5F, 0.0F, 0.25F, 1.0F, 1.5F, 3.0F, 42.0F}, 1U);
    const std::array neutral_node{
        image::AdjustmentNode{
            .node_id = "neutral-perceptual-contrast",
            .parameters = image::ContrastAdjustment{.factor = 1.0, .pivot = 0.18},
        },
    };
    const auto neutral = image::execute_adjustment_nodes(extended, neutral_node);
    expect(
        neutral.samples == extended.samples,
        "factor one is bit-exact over negative, normalized, and super-white data"
    );

    const auto grayscale = rgb_image(3, {
        0.01F,
        0.01F,
        0.01F,
        0.18F,
        0.18F,
        0.18F,
        1.0F,
        1.0F,
        1.0F,
    });
    const std::array collapsed_node{
        image::AdjustmentNode{
            .node_id = "collapsed-perceptual-contrast",
            .parameters = image::ContrastAdjustment{.factor = 0.0, .pivot = 0.18},
        },
    };
    const auto collapsed = image::execute_adjustment_nodes(grayscale, collapsed_node);
    for (const float sample : collapsed.samples) {
        expect_close(sample, 0.18F, "factor zero converges positive lightness to the public pivot");
    }
}

void one_prepared_contract_drives_cpu_and_metal_lowering() {
    const image::AdjustmentNode node{
        .node_id = "shared-prepared-perceptual-contrast",
        .parameters = image::ContrastAdjustment{.factor = 2.5, .pivot = 0.18},
    };
    const auto& parameters = std::get<image::ContrastAdjustment>(node.parameters);
    const auto prepared = image::detail::prepare_perceptual_contrast(parameters, node, 0U);
    expect(
        !prepared.neutral() && !prepared.collapses_to_pivot(),
        "a non-neutral positive factor prepares the bounded contrast curve"
    );
    expect_close_double(
        prepared.pivot_lightness(),
        std::cbrt(0.18),
        1.0e-12,
        "the prepared pivot is the Oklab-lightness anchor"
    );
    expect_close_double(
        prepared.signed_amount(),
        std::log2(2.5) * 0.20,
        1.0e-12,
        "the prepared signed amount owns the public-factor mapping"
    );

    const std::array nodes{node};
    auto input = rgb_image(1U, {0.18F, 0.25F, 0.40F});
    input.working_space = linear_srgb();
    const auto plan = image::compile_edit_execution_plan(nodes);
    const auto metal = image::detail::prepare_metal_adjustment(input, nodes, plan, {});
    expect(
        metal.program.has_value(),
        std::string("the shared contrast plan lowers to Metal: ") + metal.diagnostic
    );
    if (!metal.program.has_value()) {
        return;
    }
    expect(
        metal.program->operations.size() == 1U
            && metal.program->operations.front().opcode
                == static_cast<std::uint32_t>(
                    image::detail::MetalAdjustmentOpcode::contrast
                ),
        "Metal lowering emits exactly one contrast operation"
    );
    if (metal.program->operations.empty()) {
        return;
    }
    const auto& lowered = metal.program->operations.front().parameter_0;
    expect_close(
        lowered[0],
        static_cast<float>(prepared.pivot_lightness()),
        "Metal consumes the prepared perceptual pivot"
    );
    expect_close(
        lowered[1],
        static_cast<float>(prepared.signed_amount()),
        "Metal consumes the prepared bounded curve amount"
    );
    expect(
        lowered[2] == 0.0F,
        "Metal consumes the prepared factor-zero collapse flag"
    );
}

} // namespace

int main() {
    scene_contrast_is_restrained_and_preserves_the_middle_gray_anchor();
    neutral_and_zero_factor_contracts_are_explicit();
    one_prepared_contract_drives_cpu_and_metal_lowering();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
