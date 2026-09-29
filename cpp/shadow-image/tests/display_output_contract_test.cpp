#include <shadow/image/decoder_error.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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

[[nodiscard]] bool environment_enabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

[[nodiscard]] image::WorkingRgbSpace linear_srgb_space() {
    return image::WorkingRgbSpace{
        .id = "srgb-d65-linear",
        .primaries =
            {
                image::Chromaticity{0.6400, 0.3300},
                image::Chromaticity{0.3000, 0.6000},
                image::Chromaticity{0.1500, 0.0600},
            },
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] image::FloatRgbImage make_image(
    const image::Dimensions dimensions,
    const image::ImageReference reference,
    const std::vector<std::array<float, 3U>>& palette,
    const bool padded = false
) {
    const std::size_t packed_row_floats = static_cast<std::size_t>(dimensions.width) * 3U;
    const std::size_t stride_floats = packed_row_floats + (padded ? 5U : 0U);
    image::FloatRgbImage result{
        .dimensions = dimensions,
        .row_stride_bytes = stride_floats * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = reference,
        .working_space = linear_srgb_space(),
        .level_zero_to_raster_scale_x = 1.0,
        .level_zero_to_raster_scale_y = 1.0,
        .samples =
            std::vector<float>(static_cast<std::size_t>(dimensions.height) * stride_floats, -99.0F),
    };
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const auto& color =
                palette[(static_cast<std::size_t>(y) * dimensions.width + x) % palette.size()];
            const std::size_t index =
                static_cast<std::size_t>(y) * stride_floats + static_cast<std::size_t>(x) * 3U;
            std::copy_n(color.data(), color.size(), result.samples.data() + index);
        }
    }
    return result;
}

[[nodiscard]] image::DisplayOutputRequest request_for(
    const image::FloatRgbImage& source,
    const std::uint32_t origin_x = 0U,
    const std::uint32_t origin_y = 0U
) {
    return image::DisplayOutputRequest{
        .target_dimensions = source.dimensions,
        .output_origin_x = origin_x,
        .output_origin_y = origin_y,
    };
}

void cpu_oracle_preserves_the_scene_and_display_contracts() {
    const std::vector<std::array<float, 3U>> palette{{
        {-0.25F, -0.25F, -0.25F},
        {0.0F, 0.0F, 0.0F},
        {0.18F, 0.18F, 0.18F},
        {0.5F, 0.5F, 0.5F},
        {1.0F, 1.0F, 1.0F},
        {4.0F, 4.0F, 4.0F},
        {1.4F, -0.2F, 0.35F},
        {-0.15F, 1.3F, 0.2F},
        {0.15F, 0.1F, 2.5F},
    }};
    const auto scene = make_image({9U, 7U}, image::ImageReference::scene_referred, palette, true);
    const auto display =
        make_image(scene.dimensions, image::ImageReference::display_referred, palette, true);
    const auto scene_output = image::render_linear_srgb_to_display_srgb8_cpu_reference(
        scene,
        request_for(scene, 31U, 47U)
    );
    const auto repeated = image::render_linear_srgb_to_display_srgb8_cpu_reference(
        scene,
        request_for(scene, 31U, 47U)
    );
    const auto display_output = image::render_linear_srgb_to_display_srgb8_cpu_reference(
        display,
        request_for(display, 31U, 47U)
    );
    expect(scene_output.valid(), "CPU scene output has a complete RGB8 contract");
    expect(display_output.valid(), "CPU display output has a complete RGB8 contract");
    expect(
        scene_output.bytes == repeated.bytes,
        "CPU display output is deterministic for a fixed image-space origin"
    );
    expect(
        scene_output.bytes != display_output.bytes,
        "scene-referred input receives the scene curve while display input bypasses it"
    );
    expect(
        scene_output.bytes[2U * 3U] >= 104U && scene_output.bytes[2U * 3U] <= 105U,
        "scene-referred 18-percent gray uses LibRaw H=0's BT.709 transfer before RGB8 encoding"
    );

    // The first six palette entries are neutral. The common dither value must be shared by all
    // three channels, including black, middle gray, super-white and negative scene samples.
    for (std::size_t pixel = 0U; pixel < 6U; ++pixel) {
        const std::size_t index = pixel * 3U;
        expect(
            scene_output.bytes[index] == scene_output.bytes[index + 1U]
                && scene_output.bytes[index] == scene_output.bytes[index + 2U],
            "scene output preserves the neutral axis without chroma dither"
        );
        expect(
            display_output.bytes[index] == display_output.bytes[index + 1U]
                && display_output.bytes[index] == display_output.bytes[index + 2U],
            "display output preserves the neutral axis without chroma dither"
        );
    }
    expect(
        scene_output.bytes[0] == 0U && display_output.bytes[0] == 0U,
        "negative neutral input maps to display black"
    );
    expect(
        display_output.bytes[5U * 3U] == 255U,
        "display-referred super-white maps to display white"
    );
    expect(
        scene_output.bytes[4U * 3U] >= 235U && scene_output.bytes[4U * 3U] < 255U,
        "scene-referred unit white enters the SDR shoulder instead of a hard display clip"
    );
    expect(
        scene_output.bytes[5U * 3U] > scene_output.bytes[4U * 3U]
            && scene_output.bytes[5U * 3U] <= 255U,
        "scene-referred super-white remains ordered above unit white through the SDR shoulder"
    );
}

void coordinate_dither_is_origin_stable_and_visible() {
    const auto source =
        make_image({37U, 19U}, image::ImageReference::display_referred, {{{0.18F, 0.18F, 0.18F}}});
    const auto first = image::render_linear_srgb_to_display_srgb8_cpu_reference(
        source,
        request_for(source, 0U, 0U)
    );
    const auto shifted = image::render_linear_srgb_to_display_srgb8_cpu_reference(
        source,
        request_for(source, 10'003U, 20'011U)
    );
    expect(
        first.bytes != shifted.bytes,
        "changing the image-space origin changes the deterministic dither phase"
    );
    for (std::size_t index = 0U; index < first.bytes.size(); index += 3U) {
        expect(
            first.bytes[index] == first.bytes[index + 1U]
                && first.bytes[index] == first.bytes[index + 2U],
            "origin-keyed dither remains luminance-only"
        );
    }
}

void rgb16_uses_the_shared_mapping_without_8bit_quantization() {
    const auto source = make_image(
        {5U, 3U},
        image::ImageReference::display_referred,
        {{{0.18F, 0.18F, 0.18F}, {0.4F, 0.2F, 0.05F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}},
        true
    );
    const auto high_bit = image::render_linear_srgb_to_display_srgb16_cpu_reference(
        source,
        request_for(source, 77U, 91U)
    );
    const auto high_bit_shifted = image::render_linear_srgb_to_display_srgb16_cpu_reference(
        source,
        request_for(source, 10'077U, 20'091U)
    );
    const auto eight_bit = image::render_linear_srgb_to_display_srgb8_cpu_reference(
        source,
        request_for(source, 77U, 91U)
    );
    expect(high_bit.valid(), "CPU high-bit output has a complete packed RGB16 contract");
    expect(
        high_bit.samples == high_bit_shifted.samples,
        "RGB16 export is independent from the coordinate-keyed RGB8 dither phase"
    );
    expect(
        high_bit.row_stride_bytes
            == static_cast<std::size_t>(source.dimensions.width) * 3U * sizeof(std::uint16_t),
        "RGB16 output reports its byte stride rather than its sample stride"
    );
    expect(
        std::any_of(
            high_bit.samples.begin(),
            high_bit.samples.end(),
            [](const std::uint16_t sample) {
                return sample != 0U && sample != 65'535U && sample % 257U != 0U;
            }
        ),
        "RGB16 output retains codes that cannot be produced by expanding RGB8 samples"
    );
    for (std::size_t index = 0U; index < high_bit.samples.size(); ++index) {
        const int rounded_to_eight = static_cast<int>(
            std::floor(static_cast<double>(high_bit.samples[index]) / 257.0 + 0.5)
        );
        expect(
            std::abs(rounded_to_eight - static_cast<int>(eight_bit.bytes[index])) <= 1,
            "RGB16 and RGB8 share the same display mapping within RGB8 dither tolerance"
        );
    }
    expect(
        high_bit.samples[6U] == 0U && high_bit.samples[9U] == 65'535U,
        "RGB16 display black and white use the full integer range"
    );
}

void cpu_handles_tiny_and_odd_rasters() {
    const std::vector<std::array<float, 3U>> colors{{
        {0.2F, 0.3F, 0.4F},
        {1.2F, -0.1F, 0.7F},
        {0.001F, 0.002F, 0.003F},
    }};
    for (const auto dimensions : {
             image::Dimensions{1U, 1U},
             image::Dimensions{1U, 5U},
             image::Dimensions{7U, 1U},
             image::Dimensions{7U, 5U},
         }) {
        const auto source =
            make_image(dimensions, image::ImageReference::scene_referred, colors, true);
        const auto output = image::render_linear_srgb_to_display_srgb8_cpu_reference(
            source,
            request_for(source, 0xfffffff0U, 0xffffffe0U)
        );
        expect(output.valid(), "CPU display output accepts tiny and odd source dimensions");
    }
}

void backend_selection_falls_back_or_fails_explicitly() {
    expect(
        image::display_output_backend_identity(image::DisplayOutputBackend::cpu)
            != image::display_output_backend_identity(image::DisplayOutputBackend::metal),
        "CPU and Metal display identities remain distinct"
    );
    const auto source =
        make_image({3U, 3U}, image::ImageReference::display_referred, {{{0.1F, 0.2F, 0.3F}}});
    const auto forced_cpu = image::render_linear_srgb_to_display_srgb8_with_backend(
        source,
        request_for(source),
        image::DisplayOutputBackendMode::cpu
    );
    expect(
        forced_cpu.backend == image::DisplayOutputBackend::cpu && !forced_cpu.fell_back
            && forced_cpu.diagnostic.empty(),
        "forced CPU reports a direct CPU display result without fallback metadata"
    );
    const auto automatic = image::render_linear_srgb_to_display_srgb8_with_backend(
        source,
        request_for(source),
        image::DisplayOutputBackendMode::automatic
    );
    expect(automatic.valid(), "automatic display selection always returns a valid backend");
    if (!image::display_output_backend_available(image::DisplayOutputBackend::metal)) {
        expect(
            automatic.backend == image::DisplayOutputBackend::cpu && automatic.fell_back
                && !automatic.diagnostic.empty() && automatic.bytes == forced_cpu.bytes,
            "automatic selection restarts the complete display stage on CPU with a diagnostic"
        );
        try {
            static_cast<void>(image::render_linear_srgb_to_display_srgb8_with_backend(
                source,
                request_for(source),
                image::DisplayOutputBackendMode::metal
            ));
            expect(false, "forced Metal fails when the runtime backend is unavailable");
        } catch (const image::DecodeError& error) {
            expect(
                error.code() == image::DecodeErrorCode::internal,
                "unavailable forced Metal returns a typed backend failure"
            );
        }
    } else {
        expect(
            automatic.backend == image::DisplayOutputBackend::metal && !automatic.fell_back
                && automatic.diagnostic.empty(),
            "automatic selection reports direct Metal when the runtime is available"
        );
    }
}

void metal_stays_within_the_cpu_oracle_contract() {
    if (!image::display_output_backend_available(image::DisplayOutputBackend::metal)) {
        if (environment_enabled("SHADOW_TEST_REQUIRE_METAL")) {
            const auto source = make_image(
                {1U, 1U},
                image::ImageReference::display_referred,
                {{{0.1F, 0.2F, 0.3F}}}
            );
            try {
                static_cast<void>(image::render_linear_srgb_to_display_srgb8_with_backend(
                    source,
                    request_for(source),
                    image::DisplayOutputBackendMode::metal
                ));
            } catch (const image::DecodeError& error) {
                std::cerr << "Metal display diagnostic: " << error.what() << '\n';
            }
            expect(
                false,
                "Metal was required for display validation but no Metal backend is available"
            );
        }
        return;
    }
    std::vector<std::array<float, 3U>> palette{{
        {-0.25F, -0.25F, -0.25F},
        {0.0F, 0.0F, 0.0F},
        {0.0031307F, 0.0031308F, 0.0031309F},
        {0.003F, 0.003F, 0.003F},
        {0.18F, 0.18F, 0.18F},
        {0.5F, 0.5F, 0.5F},
        {1.0F, 1.0F, 1.0F},
        {4.0F, 4.0F, 4.0F},
        {1.4F, -0.2F, 0.35F},
        {-0.15F, 1.3F, 0.2F},
        {0.15F, 0.1F, 2.5F},
        {0.9F, 0.04F, 0.7F},
    }};
    // Fixed pseudo-random extended-gamut samples exercise many OETF code boundaries and Oklab
    // hue slices without turning parity into a non-reproducible fuzz test.
    std::uint32_t state = 0x91e10da5U;
    const auto next_sample = [&state]() {
        state = state * 1'664'525U + 1'013'904'223U;
        const float unit = static_cast<float>(state >> 8U) / static_cast<float>(0x00ffffffU);
        return unit * 4.5F - 0.5F;
    };
    for (std::size_t index = 0U; index < 512U; ++index) {
        palette.push_back({next_sample(), next_sample(), next_sample()});
    }
    for (const auto reference : {
             image::ImageReference::scene_referred,
             image::ImageReference::display_referred,
         }) {
        for (const auto dimensions : {
                 image::Dimensions{1U, 1U},
                 image::Dimensions{1U, 5U},
                 image::Dimensions{7U, 1U},
                 image::Dimensions{31U, 19U},
             }) {
            const auto source = make_image(dimensions, reference, palette, true);
            const auto request = request_for(source, 43U, 71U);
            const auto cpu =
                image::render_linear_srgb_to_display_srgb8_cpu_reference(source, request);
            const auto metal = image::render_linear_srgb_to_display_srgb8_with_backend(
                source,
                request,
                image::DisplayOutputBackendMode::metal
            );
            const auto repeated = image::render_linear_srgb_to_display_srgb8_with_backend(
                source,
                request,
                image::DisplayOutputBackendMode::metal
            );
            expect(metal.valid(), "forced Metal output has a complete RGB8 contract");
            expect(
                metal.backend == image::DisplayOutputBackend::metal,
                "forced Metal records its effective display backend"
            );
            expect(
                metal.bytes == repeated.bytes,
                "Metal display output is byte deterministic across repeated execution"
            );
            std::vector<std::uint8_t> differences;
            differences.reserve(cpu.bytes.size());
            std::uint64_t total = 0U;
            for (std::size_t index = 0U; index < cpu.bytes.size(); ++index) {
                const auto difference = static_cast<std::uint8_t>(std::abs(
                    static_cast<int>(cpu.bytes[index]) - static_cast<int>(metal.bytes[index])
                ));
                differences.push_back(difference);
                total += difference;
            }
            std::sort(differences.begin(), differences.end());
            const std::size_t p99_index =
                differences.empty() ? 0U : (differences.size() - 1U) * 99U / 100U;
            const auto maximum = differences.empty() ? 0U : differences.back();
            const auto p99 = differences.empty() ? 0U : differences[p99_index];
            const double mean = differences.empty() ? 0.0
                                                    : static_cast<double>(total)
                                                          / static_cast<double>(differences.size());
            expect(maximum <= 2U, "Metal display maximum error stays within two RGB8 codes");
            expect(p99 <= 1U, "Metal display p99 error stays within one RGB8 code");
            expect(mean <= 0.10, "Metal display mean error stays below 0.10 RGB8 codes");
        }
    }
}

void invalid_contracts_fail_before_backend_selection() {
    expect(
        !image::DisplayRgb8Image{
            .dimensions =
                {
                    std::numeric_limits<std::uint32_t>::max(),
                    std::numeric_limits<std::uint32_t>::max(),
                },
            .row_stride_bytes = 0U,
            .bytes = {},
            .backend = image::DisplayOutputBackend::cpu,
        }
             .valid(),
        "RGB8 result validation rejects a pixel-count multiplication overflow"
    );
    expect(
        !image::DisplayRgb16Image{
            .dimensions =
                {
                    std::numeric_limits<std::uint32_t>::max(),
                    std::numeric_limits<std::uint32_t>::max(),
                },
            .row_stride_bytes = 0U,
            .samples = {},
        }
             .valid(),
        "RGB16 result validation rejects a pixel-count multiplication overflow"
    );

    auto source =
        make_image({3U, 2U}, image::ImageReference::scene_referred, {{{0.1F, 0.2F, 0.3F}}});
    auto wrong_size = request_for(source);
    wrong_size.target_dimensions = {2U, 2U};
    try {
        static_cast<void>(image::render_linear_srgb_to_display_srgb8_with_backend(
            source,
            wrong_size,
            image::DisplayOutputBackendMode::metal
        ));
        expect(false, "display output rejects a v1 resize request");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported_layout,
            "v1 resize rejection is a typed layout error"
        );
    }

    source.samples[0] = std::numeric_limits<float>::quiet_NaN();
    try {
        static_cast<void>(image::render_linear_srgb_to_display_srgb8_with_backend(
            source,
            request_for(source),
            image::DisplayOutputBackendMode::automatic
        ));
        expect(false, "display output rejects non-finite source samples");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::internal,
            "non-finite display input uses the existing edited-proxy failure class"
        );
    }
}

} // namespace

int main() {
    cpu_oracle_preserves_the_scene_and_display_contracts();
    coordinate_dither_is_origin_stable_and_visible();
    rgb16_uses_the_shared_mapping_without_8bit_quantization();
    cpu_handles_tiny_and_odd_rasters();
    backend_selection_falls_back_or_fails_explicitly();
    metal_stays_within_the_cpu_oracle_contract();
    invalid_contracts_fail_before_backend_selection();
    return failures == 0 ? 0 : 1;
}
