#include "contract_test_assertions.hpp"

#include "optics/metal_manual_optics.hpp"

#include <shadow/image/optics.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

class EnvironmentOverride final {
  public:
    EnvironmentOverride(const char* name, const char* value) : name_(name) {
        if (const char* existing = std::getenv(name); existing != nullptr) {
            previous_ = existing;
        }
        if (value == nullptr) {
            unsetenv(name);
        } else {
            setenv(name, value, 1);
        }
    }

    ~EnvironmentOverride() {
        if (previous_.has_value()) {
            setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    EnvironmentOverride(const EnvironmentOverride&) = delete;
    EnvironmentOverride& operator=(const EnvironmentOverride&) = delete;

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

[[nodiscard]] bool environment_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

[[nodiscard]] image::SceneLinearRgbFrame synthetic_frame(const image::Dimensions dimensions) {
    image::SceneLinearRgbFrame frame;
    frame.dimensions = dimensions;
    frame.row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float);
    frame.samples.resize(static_cast<std::size_t>(dimensions.width) * dimensions.height * 3U);
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const float horizontal = static_cast<float>(x) / static_cast<float>(dimensions.width);
            const float vertical = static_cast<float>(y) / static_cast<float>(dimensions.height);
            const std::size_t index = (static_cast<std::size_t>(y) * dimensions.width + x) * 3U;
            frame.samples[index] = 0.08F + 1.7F * horizontal;
            frame.samples[index + 1U] = 0.12F + 1.3F * vertical;
            frame.samples[index + 2U] = 0.04F + 0.9F * horizontal + 0.7F * vertical;
        }
    }
    return frame;
}

[[nodiscard]] image::OpticsSettings manual_settings() {
    auto settings = image::default_optics_settings();
    settings.enabled = false;
    settings.manual_distortion = 38;
    settings.manual_tca_red_cyan = -21;
    settings.manual_tca_blue_yellow = 17;
    settings.manual_vignetting_amount = 42;
    settings.manual_vignetting_midpoint = 37U;
    return settings;
}

[[nodiscard]] image::SceneLinearRgbFrame render(
    const image::OpticsProvider& provider,
    const image::SceneLinearRgbFrame& input,
    const image::OpticsSettings& settings,
    const char* acceleration
) {
    EnvironmentOverride backend("SHADOW_IMAGE_ACCELERATION", acceleration);
    const auto result =
        provider.correct_scene_linear_reference(input, image::AssetMetadata{}, settings);
    if (!result.corrected_scene_linear_rgb.has_value()) {
        throw std::runtime_error("manual optics did not materialize a scene-linear frame");
    }
    return *result.corrected_scene_linear_rgb;
}

template <typename Callable>
[[nodiscard]] double median_milliseconds(const std::size_t iterations, Callable&& callable) {
    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        callable();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        samples.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2U];
}

void metal_manual_optics_matches_the_cpu_oracle() {
    if (!image::detail::metal_manual_scene_linear_optics_available()) {
        if (environment_enabled("SHADOW_TEST_REQUIRE_METAL")) {
            throw std::runtime_error("Metal manual optics is required but unavailable");
        }
        return;
    }
    const auto provider = image::make_lensfun_optics_provider();
    const auto input = synthetic_frame({97U, 73U});
    const auto settings = manual_settings();
    const auto cpu = render(*provider, input, settings, "cpu");
    EnvironmentOverride tiny_tiles("SHADOW_TEST_METAL_OPTICS_TILE_BYTES", "1");
    const auto metal = render(*provider, input, settings, "metal");
    const auto repeated = render(*provider, input, settings, "metal");
    expect(
        metal.valid() && metal.dimensions == input.dimensions,
        "Metal manual optics preserves the scene-linear frame layout"
    );
    expect(
        metal.samples == repeated.samples,
        "Metal manual optics is byte deterministic across repeated tiled execution"
    );

    float maximum_error = 0.0F;
    double total_error = 0.0;
    for (std::size_t index = 0U; index < cpu.samples.size(); ++index) {
        const float error = std::abs(cpu.samples[index] - metal.samples[index]);
        maximum_error = std::max(maximum_error, error);
        total_error += error;
    }
    const double mean_error = total_error / static_cast<double>(cpu.samples.size());
    expect(
        maximum_error <= 2.0e-5F,
        "Metal manual optics stays within the fp32 CPU maximum tolerance"
    );
    expect(mean_error <= 2.0e-6, "Metal manual optics stays within the fp32 CPU mean tolerance");
}

void benchmark_manual_optics_when_requested() {
    if (!environment_enabled("SHADOW_TEST_MANUAL_OPTICS_METAL_BENCHMARK")) {
        return;
    }
    if (!image::detail::metal_manual_scene_linear_optics_available()) {
        throw std::runtime_error("Metal is unavailable for the manual-optics benchmark");
    }
    EnvironmentOverride ordinary_tiles("SHADOW_TEST_METAL_OPTICS_TILE_BYTES", nullptr);
    const auto provider = image::make_lensfun_optics_provider();
    const image::Dimensions dimensions{3'000U, 2'000U};
    const auto input = synthetic_frame(dimensions);
    const auto settings = manual_settings();
    std::uint64_t checksum = 0U;
    constexpr std::size_t iterations = 3U;
    const double cpu = median_milliseconds(iterations, [&]() {
        const auto output = render(*provider, input, settings, "cpu");
        checksum +=
            static_cast<std::uint64_t>(output.samples[output.samples.size() / 2U] * 1'000.0F);
    });
    const double metal = median_milliseconds(iterations, [&]() {
        const auto output = render(*provider, input, settings, "metal");
        checksum +=
            static_cast<std::uint64_t>(output.samples[output.samples.size() / 2U] * 1'000.0F);
    });
    std::cout << std::fixed << std::setprecision(3) << "BENCH manual optics " << dimensions.width
              << 'x' << dimensions.height << " CPU=" << cpu << "ms"
              << " Metal=" << metal << "ms"
              << " speedup=" << cpu / metal << 'x' << " checksum=" << checksum << '\n';
}

} // namespace

int main() {
    metal_manual_optics_matches_the_cpu_oracle();
    benchmark_manual_optics_when_requested();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
