#include "contract_test_assertions.hpp"
#include "processed_rgb_session_fixture.hpp"
#include "scoped_environment.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <shadow/image/warm_edit_preview.hpp>
#include <stop_token>
using namespace shadow::image;
using namespace shadow::image::test_support;
int main() {
    auto pixels = processed_linear_gradient(600, 400);
    pixels.reference = RgbBufferReference::decoded_raster;
    for (std::size_t i = 0; i < pixels.samples.size(); i += 3) {
        pixels.samples[i] = 8192;
        pixels.samples[i + 1] = 16384;
        pixels.samples[i + 2] = 32768;
    }
    RetainedRgbSession source(std::move(pixels));
    auto warm = prepare_warm_edit_preview(source, 600);
    const std::array layers{AdjustmentLayer{
        .layer_id = "prefix",
        .nodes = {
            AdjustmentNode{.node_id = "upstream", .parameters = ExposureAdjustment{.stops = 1.0}}
        }
    }};
    std::array<CurveInputMap, 5> cpu;
    {
        ScopedEnvironment force("SHADOW_IMAGE_ACCELERATION", "cpu");
        for (std::uint8_t channel = 0; channel < 5; ++channel) {
            cpu[channel] = warm.curve_input_map(layers, channel, {}, nullptr);
            expect(
                cpu[channel].dimensions.width == 512 && cpu[channel].dimensions.height == 341,
                "bounded canvas map"
            );
            expect(cpu[channel].values.size() == 512 * 341, "complete map cells");
        }
    }
    const auto encode = [](double v) { return 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; };
    expect(
        std::abs(cpu[2].values[0] - encode(16384.0 / 65535)) < 0.0001,
        "red samples after upstream exposure in encoded curve space"
    );
    expect(std::abs(cpu[3].values[0] - encode(32768.0 / 65535)) < 0.0001, "green input space");
    expect(
        std::abs(cpu[4].values[0] - encode(65536.0 / 65535)) < 0.0001,
        "HDR sample remains unclamped"
    );
    expect(
        std::abs(cpu[1].values[0] - (cpu[2].values[0] + cpu[3].values[0] + cpu[4].values[0]) / 3)
            < 0.0001,
        "master representative tone"
    );
    if (warm.gpu_stats().resident) {
        for (std::uint8_t channel = 0; channel < 5; ++channel) {
            auto accelerated = warm.curve_input_map(layers, channel, {}, nullptr);
            expect(accelerated.values.size() == cpu[channel].values.size(), "GPU map shape");
            for (std::size_t i = 0; i < accelerated.values.size(); ++i)
                if (std::abs(accelerated.values[i] - cpu[channel].values[i]) > 0.0002) {
                    expect(false, "CPU/Metal input map parity");
                    break;
                }
        }
    } else
        expect(!std::getenv("SHADOW_TEST_REQUIRE_METAL"), "required Metal resident session");
    std::stop_source stop;
    stop.request_stop();
    expect(
        warm.curve_input_map(layers, 0, {}, nullptr, stop.get_token()).values.empty(),
        "cancelled request has no map"
    );
    auto gradient_pixels = processed_linear_gradient(96, 64);
    gradient_pixels.reference = RgbBufferReference::decoded_raster;
    RetainedRgbSession gradient_source(std::move(gradient_pixels));
    auto gradient = prepare_warm_edit_preview(gradient_source, 96);
    const PhotoGeometry crop{.crop_left = 0.5, .quarter_turn = PhotoQuarterTurn::clockwise_90};
    const std::array neutral{AdjustmentLayer{
        .layer_id = "neutral",
        .nodes = {
            AdjustmentNode{.node_id = "identity", .parameters = ExposureAdjustment{.stops = 0}}
        }
    }};
    auto canvas = gradient.curve_input_map(neutral, 2, crop, nullptr);
    expect(
        canvas.dimensions.width == 64 && canvas.dimensions.height == 48,
        "map follows cropped and rotated display dimensions"
    );
    expect(
        canvas.values.front() > 0.7 && canvas.values.back() > 0.99,
        "display top and bottom sample the correct original red gradient"
    );
    FakeRgbSession counted;
    auto cached = prepare_warm_edit_preview(counted, 8);
    for (std::uint8_t c = 0; c < 5; ++c)
        (void)cached.curve_input_map(layers, c, {}, nullptr);
    expect(counted.reference_render_count() == 1, "sampling never reopens source");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
