#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void selective_color_has_distinct_relative_absolute_and_neutral_semantics() {
    auto input = rgb_image(2, {0.70F, 0.20F, 0.20F, 1.0F, 1.0F, 1.0F});
    input.working_space = linear_srgb();

    image::PerceptualColorAdjustment relative;
    relative.selective_color_relative = true;
    // Add magenta to Reds and black to Whites. Relative adjustment must leave
    // specular white untouched because its CMYK components are all zero.
    relative.selective_color_cmyk[0][1] = 0.25;
    relative.selective_color_cmyk[6][3] = 0.25;
    const std::array relative_node{
        image::AdjustmentNode{
            .node_id = "selective-color-relative",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = relative,
        },
    };
    const auto relative_output = image::execute_adjustment_nodes(input, relative_node);
    expect(relative_output.samples[1] < input.samples[1],
           "Relative Selective Color adds magenta by reducing green in a red target");
    expect(relative_output.samples[3] == input.samples[3] &&
               relative_output.samples[4] == input.samples[4] &&
               relative_output.samples[5] == input.samples[5],
           "Relative Selective Color cannot tint pure specular white");

    image::PerceptualColorAdjustment absolute = relative;
    absolute.selective_color_relative = false;
    const std::array absolute_node{
        image::AdjustmentNode{
            .node_id = "selective-color-absolute",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = absolute,
        },
    };
    const auto absolute_output = image::execute_adjustment_nodes(input, absolute_node);
    expect(absolute_output.samples[1] < relative_output.samples[1],
           "Absolute Selective Color applies a stronger fixed magenta change than Relative");
    expect(absolute_output.samples[3] < input.samples[3] &&
               absolute_output.samples[4] < input.samples[4] &&
               absolute_output.samples[5] < input.samples[5],
           "Absolute Selective Color can add black to pure white");
}

void selective_color_routes_every_target_and_protects_oklab_lightness() {
    constexpr std::size_t target_count = image::selective_color_target_count;
    const std::array<std::array<float, 3>, target_count> target_samples{{
        {1.0F, 0.0F, 0.0F},
        {1.0F, 1.0F, 0.0F},
        {0.0F, 1.0F, 0.0F},
        {0.0F, 1.0F, 1.0F},
        {0.0F, 0.0F, 1.0F},
        {1.0F, 0.0F, 1.0F},
        {1.0F, 1.0F, 1.0F},
        {0.125F, 0.125F, 0.125F},
        {0.0F, 0.0F, 0.0F},
    }};
    std::vector<float> samples;
    samples.reserve(target_count * 3U);
    for (const auto& sample : target_samples) {
        samples.insert(samples.end(), sample.begin(), sample.end());
    }
    auto input = rgb_image(static_cast<std::uint32_t>(target_count), std::move(samples));
    input.working_space = linear_srgb();

    for (std::size_t target = 0U; target < target_count; ++target) {
        image::PerceptualColorAdjustment parameters;
        parameters.selective_color_relative = false;
        parameters.selective_color_cmyk[target][3] = target == 8U ? -0.25 : 0.25;
        const std::array nodes{
            image::AdjustmentNode{
                .node_id = "selective-color-target-routing",
                .parameter_schema_version = image::perceptual_color_parameter_schema_version,
                .implementation_version = image::perceptual_color_implementation_version,
                .parameters = parameters,
            },
        };
        const auto output = image::execute_adjustment_nodes(input, nodes);
        std::array<double, target_count> responses{};
        for (std::size_t pixel = 0U; pixel < target_count; ++pixel) {
            const std::size_t base = pixel * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                responses[pixel] += std::abs(static_cast<double>(output.samples[base + channel] -
                                                                 input.samples[base + channel]));
            }
        }
        const auto strongest = std::max_element(responses.begin(), responses.end());
        expect(static_cast<std::size_t>(strongest - responses.begin()) == target,
               "Selective Color routes each of its six hues and three achromatic targets");
        expect(responses[target] > 1.0e-3,
               "every Selective Color target has an observable absolute CMYK response");
    }

    auto lightness_input = rgb_image(1, {0.70F, 0.20F, 0.10F});
    lightness_input.working_space = linear_srgb();
    image::PerceptualColorAdjustment unprotected;
    unprotected.selective_color_relative = false;
    unprotected.selective_color_cmyk[0][1] = 0.5;
    image::PerceptualColorAdjustment protected_color = unprotected;
    protected_color.selective_color_lightness_protection = 1.0;
    const auto make_node = [](const std::string_view id,
                              const image::PerceptualColorAdjustment& parameters) {
        return image::AdjustmentNode{
            .node_id = std::string(id),
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = parameters,
        };
    };
    const std::array unprotected_nodes{
        make_node("unprotected-selective-color", unprotected),
    };
    const std::array protected_nodes{
        make_node("protected-selective-color", protected_color),
    };
    const auto unprotected_output =
        image::execute_adjustment_nodes(lightness_input, unprotected_nodes);
    const auto protected_output = image::execute_adjustment_nodes(lightness_input, protected_nodes);
    const auto input_lab = oklab_from_linear_srgb({0.70F, 0.20F, 0.10F});
    const auto unprotected_lab = oklab_from_linear_srgb({
        unprotected_output.samples[0],
        unprotected_output.samples[1],
        unprotected_output.samples[2],
    });
    const auto protected_lab = oklab_from_linear_srgb({
        protected_output.samples[0],
        protected_output.samples[1],
        protected_output.samples[2],
    });
    expect(std::abs(unprotected_lab[0] - input_lab[0]) > 1.0e-3,
           "unprotected Selective Color retains its Photoshop-style lightness change");
    expect_close_double(protected_lab[0], input_lab[0], 2.0e-5,
                        "full Selective Color lightness protection restores source Oklab L");
    expect(std::abs(protected_lab[1] - input_lab[1]) > 1.0e-3 ||
               std::abs(protected_lab[2] - input_lab[2]) > 1.0e-3,
           "Selective Color lightness protection retains the authored hue/chroma correction");
}

} // namespace

int main() {
    selective_color_has_distinct_relative_absolute_and_neutral_semantics();
    selective_color_routes_every_target_and_protects_oklab_lightness();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
