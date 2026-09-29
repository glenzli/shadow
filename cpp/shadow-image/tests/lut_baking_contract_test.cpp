#include <shadow/image/lut_baking.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stop_token>
#include <stdexcept>

namespace image = shadow::image;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template<class Function>
void rejects(Function function, const char* message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}
} // namespace

int main() {
    std::array layers{image::AdjustmentLayer{
        .layer_id = "half-exposure",
        .opacity = 0.5,
        .nodes = {image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{.stops = 1.0},
        }},
    }};
    const auto baked = image::bake_cube_lut(layers, 17);
    const auto round_trip = image::parse_cube_lut(image::serialize_baked_cube_lut(baked));
    const auto sampled = image::sample_cube_lut(round_trip, {0.2F, 0.4F, 0.8F});
    require(std::abs(sampled[0] - 0.3F) < 1e-6F
            && std::abs(sampled[1] - 0.6F) < 1e-6F
            && std::abs(sampled[2] - 1.2F) < 1e-6F,
        "LUT baking lost node strength or clipped super-white output");
    require(baked.probe_count == 4096 && baked.maximum_absolute_error < 1e-6,
        "linear transform did not pass independent probes");
    rejects([&] { static_cast<void>(image::bake_cube_lut(layers, 128)); },
        "unsupported lattice size was admitted");
    std::stop_source cancelled;
    cancelled.request_stop();
    rejects([&] { static_cast<void>(image::bake_cube_lut(layers, 65, cancelled.get_token())); },
        "cancelled bake executed");

    layers[0].mask = image::LocalMask{};
    rejects([&] { static_cast<void>(image::bake_cube_lut(layers, 17)); },
        "spatial mask was silently flattened");
    layers[0].mask.reset();
    layers[0].nodes[0].parameters = image::SelectiveToneAdjustment{.shadows = 0.3};
    rejects([&] { static_cast<void>(image::bake_cube_lut(layers, 17)); },
        "guided tone was silently baked as pixel-local");

    image::SharpenAdjustment spatial_grading;
    spatial_grading.execution_pass = image::DetailEffectsExecutionPass::color_grading;
    spatial_grading.texture = 0.2;
    layers[0].nodes[0].parameters = spatial_grading;
    rejects([&] { static_cast<void>(image::bake_cube_lut(layers, 17)); },
        "spatial texture inside the grading pass was silently baked");

    layers[0].nodes[0].parameters = image::OklabLightnessToneCurve{
        .lightness = {.points = {{0.0, 0.0}, {0.3, 0.18}, {0.7, 0.8}, {1.0, 1.0}}},
    };
    const auto coarse = image::bake_cube_lut(layers, 17);
    const auto fine = image::bake_cube_lut(layers, 33);
    require(coarse.root_mean_square_error > 0.0
            && fine.root_mean_square_error < coarse.root_mean_square_error,
        "nonlinear interpolation error was hidden or did not improve with more samples");
    std::cout << "LUT baking contract passed\n";
}
