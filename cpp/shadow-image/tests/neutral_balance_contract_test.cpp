#include <shadow/image/neutral_balance.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] image::WorkingRgbSpace linear_srgb() {
    return image::WorkingRgbSpace{
        .id = "linear-srgb-d65",
        .primaries = {{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}},
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] image::FloatRgbImage make_image(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::vector<std::array<float, 3U>>& palette,
    const image::ImageReference reference = image::ImageReference::scene_referred
) {
    const std::size_t stride_floats = static_cast<std::size_t>(width) * 3U + 2U;
    image::FloatRgbImage result{
        .dimensions = {width, height},
        .row_stride_bytes = stride_floats * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = reference,
        .working_space = linear_srgb(),
        .samples = std::vector<float>(stride_floats * height, -91.0F),
    };
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const auto& value = palette[(static_cast<std::size_t>(y) * width + x) % palette.size()];
            const std::size_t offset = static_cast<std::size_t>(y) * stride_floats + x * 3U;
            result.samples[offset] = value[0];
            result.samples[offset + 1U] = value[1];
            result.samples[offset + 2U] = value[2];
        }
    }
    return result;
}

[[nodiscard]] bool close(const double actual, const double expected, const double tolerance = 1.0e-6) {
    return std::abs(actual - expected) <= tolerance;
}

void neutral_and_modestly_cast_surfaces_produce_evidence() {
    const auto neutral = make_image(
        16U,
        12U,
        {{
            {0.18F, 0.18F, 0.18F},
            {0.40F, 0.40F, 0.40F},
            {0.20F, 0.06F, 0.04F},
            {0.03F, 0.24F, 0.08F},
        }}
    );
    const auto neutral_analysis = image::analyze_scene_linear_neutral_balance(neutral);
    expect(
        neutral_analysis.available && neutral_analysis.eligible_pixel_count >= 8U
            && !neutral_analysis.candidates.empty()
            && close(neutral_analysis.red_to_green, 1.0)
            && close(neutral_analysis.blue_to_green, 1.0)
            && close(neutral_analysis.suggested_red_gain, 1.0)
            && close(neutral_analysis.suggested_blue_gain, 1.0),
        "near-neutral surfaces remain neutral despite saturated color outliers"
    );

    const auto modest_cast = make_image(
        16U,
        12U,
        {{
            {0.21F, 0.20F, 0.19F},
            {0.42F, 0.40F, 0.38F},
        }}
    );
    const auto cast_analysis = image::analyze_scene_linear_neutral_balance(modest_cast);
    expect(
        cast_analysis.available
            && close(cast_analysis.red_to_green, 1.05, 2.0e-5)
            && close(cast_analysis.blue_to_green, 0.95, 2.0e-5)
            && close(cast_analysis.suggested_red_gain, 1.0 / 1.05, 2.0e-5)
            && close(cast_analysis.suggested_blue_gain, 1.0 / 0.95, 2.0e-5),
        "a modest neutral-surface cast yields inverse RGB gain evidence without editing the image"
    );
}

void weak_or_invalid_evidence_fails_closed() {
    const auto saturated = make_image(
        8U,
        8U,
        {{
            {0.70F, 0.08F, 0.04F},
            {0.03F, 0.55F, 0.09F},
        }}
    );
    const auto saturated_analysis = image::analyze_scene_linear_neutral_balance(saturated);
    expect(
        !saturated_analysis.available && saturated_analysis.eligible_pixel_count == 0U,
        "a scene without neutral evidence does not invent a color-cast suggestion"
    );

    const auto display = make_image(
        8U,
        8U,
        {{{0.18F, 0.18F, 0.18F}}},
        image::ImageReference::display_referred
    );
    try {
        static_cast<void>(image::analyze_scene_linear_neutral_balance(display));
        expect(false, "display-referred data is rejected instead of treated as scene evidence");
    } catch (const std::invalid_argument&) {
        expect(true, "display-referred data reports a typed input contract error");
    }

    auto malformed_options = image::NeutralBalanceAnalysisOptions{};
    malformed_options.maximum_candidate_count = 0U;
    try {
        static_cast<void>(image::analyze_scene_linear_neutral_balance(saturated, malformed_options));
        expect(false, "invalid neutral-analysis options are rejected");
    } catch (const std::invalid_argument&) {
        expect(true, "invalid neutral-analysis options report a typed input contract error");
    }
}

} // namespace

int main() {
    neutral_and_modestly_cast_surfaces_produce_evidence();
    weak_or_invalid_evidence_fails_closed();
    if (failures != 0) {
        std::cerr << failures << " neutral-balance contract checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "neutral-balance contract checks passed\n";
    return EXIT_SUCCESS;
}
