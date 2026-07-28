#include "dcp_color_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/fused_raw_development.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::ScopedEnvironment;
using shadow::image::test_support::expect;
using shadow::image::test_support::expect_close;
using shadow::image::test_support::profile_definition;
using shadow::image::test_support::raw_descriptor;

[[nodiscard]] image::DcpHsvTable value_scale_table(const float saturated_value_scale) {
    return image::DcpHsvTable{
        .hue_divisions = 1U,
        .saturation_divisions = 2U,
        .value_divisions = 1U,
        .encoding = image::DcpTableEncoding::linear,
        .entries = {
            image::DcpHsvDelta{.hue_shift_degrees = 0.0F, .saturation_scale = 1.0F, .value_scale = 1.0F},
            image::DcpHsvDelta{.hue_shift_degrees = 0.0F, .saturation_scale = 1.0F, .value_scale = saturated_value_scale},
        },
    };
}

[[nodiscard]] image::PixelBuffer one_linear_srgb_pixel(
    const std::uint16_t red,
    const std::uint16_t green,
    const std::uint16_t blue
) {
    image::PixelBuffer pixel;
    pixel.dimensions = image::Dimensions{1U, 1U};
    pixel.bits_per_channel = 16U;
    pixel.channels = 3U;
    pixel.row_stride_bytes = 3U * sizeof(std::uint16_t);
    pixel.primaries = image::RgbPrimaries::srgb_rec709_d65;
    pixel.transfer_function = image::RgbTransferFunction::linear;
    pixel.reference = image::RgbBufferReference::processed_raw;
    pixel.samples = {red, green, blue};
    return pixel;
}

[[nodiscard]] image::SceneLinearRgbFrame one_scene_linear_srgb_pixel(
    const float red,
    const float green,
    const float blue
) {
    return image::SceneLinearRgbFrame{
        .dimensions = image::Dimensions{1U, 1U},
        .row_stride_bytes = 3U * sizeof(float),
        .samples = {red, green, blue},
    };
}

void standard_dcp_rendering_stages_compile_and_apply() {
    auto definition = profile_definition(true);
    definition.profile.calibration1.hue_sat_map = value_scale_table(0.8F);
    definition.profile.look_table = value_scale_table(0.7F);
    definition.profile.tone_curve = {
        image::DcpToneCurvePoint{0.0F, 0.0F},
        image::DcpToneCurvePoint{0.5F, 0.35F},
        image::DcpToneCurvePoint{1.0F, 1.0F},
    };
    const auto transform = image::compile_dcp_color_transform(definition, raw_descriptor());
    expect(
        transform.valid() && transform.has_post_matrix_stages()
            && transform.receipt.hue_sat_map_applied
            && transform.receipt.look_table_applied
            && transform.receipt.tone_curve_applied,
        "standard DCP input-rendering stages compile into one valid camera transform"
    );
    auto pixel = one_linear_srgb_pixel(60'000U, 20'000U, 2'000U);
    static_cast<void>(image::apply_dcp_color_rendering_stages(pixel, transform));
    expect(
        pixel.samples[0] < 60'000U && pixel.samples[1] < 20'000U,
        "HueSatMap, LookTable, and ToneCurve affect camera rendering before the edit graph"
    );
    const std::string receipt_identity = image::dcp_color_receipt_identity(transform.receipt);
    expect(
        receipt_identity.find("huesat=applied") != std::string::npos
            && receipt_identity.find("look=applied") != std::string::npos
            && receipt_identity.find("tone=applied") != std::string::npos,
        "camera-rendering cache identity records every applied DCP stage"
    );
}

void scene_linear_dcp_stages_preserve_highlight_headroom() {
    auto definition = profile_definition(true);
    definition.profile.calibration1.hue_sat_map = value_scale_table(1.0F);
    const auto transform = image::compile_dcp_color_transform(definition, raw_descriptor());
    auto pixel = one_scene_linear_srgb_pixel(1.5F, 1.0F, 0.5F);
    static_cast<void>(image::apply_dcp_color_rendering_stages(pixel, transform));
    expect(
        pixel.valid() && pixel.samples[0] > 1.35F,
        "scene-linear DCP rendering keeps super-white RAW headroom"
    );
    expect_close(
        static_cast<double>(pixel.samples[1] / pixel.samples[0]),
        2.0 / 3.0,
        2.0e-3,
        "identity DCP HueSatMap preserves the normalized HDR colour ratio"
    );
}

void large_scene_linear_dcp_stage_matches_the_single_pixel_reference() {
    auto definition = profile_definition(true);
    definition.profile.calibration1.hue_sat_map = value_scale_table(0.8F);
    definition.profile.look_table = value_scale_table(0.7F);
    definition.profile.tone_curve = {
        image::DcpToneCurvePoint{0.0F, 0.0F},
        image::DcpToneCurvePoint{0.5F, 0.35F},
        image::DcpToneCurvePoint{1.0F, 1.0F},
    };
    const auto transform = image::compile_dcp_color_transform(definition, raw_descriptor());
    auto expected = one_scene_linear_srgb_pixel(0.91F, 0.37F, 0.08F);
    static_cast<void>(image::apply_dcp_color_rendering_stages(expected, transform));

    // This exceeds the DCP work partitioning threshold. Every pixel begins
    // with exactly the same data, so any split/exception ordering issue would
    // be visible as a result that differs from the known single-pixel oracle.
    image::SceneLinearRgbFrame frame{
        .dimensions = image::Dimensions{256U, 192U},
        .row_stride_bytes = 256U * 3U * sizeof(float),
    };
    frame.samples.reserve(static_cast<std::size_t>(frame.dimensions.width)
        * frame.dimensions.height * 3U);
    for (std::size_t pixel = 0U;
         pixel < static_cast<std::size_t>(frame.dimensions.width) * frame.dimensions.height;
         ++pixel) {
        frame.samples.insert(frame.samples.end(), {0.91F, 0.37F, 0.08F});
    }
    static_cast<void>(image::apply_dcp_color_rendering_stages(frame, transform));
    expect(frame.valid(), "large scene-linear DCP result retains the frame contract");
    for (std::size_t index = 0U; index < frame.samples.size(); ++index) {
        expect_close(
            frame.samples[index],
            expected.samples[index % 3U],
            1.0e-6,
            "parallel DCP post stages preserve the serial per-pixel result"
        );
    }
}

void scene_linear_dcp_metal_matches_the_cpu_reference_when_available() {
    expect(
        image::dcp_color_execution_backend_identity(image::DcpColorExecutionBackend::cpu)
            == "dcp-executor=cpu-v1;math=f64-reference",
        "DCP cache identity identifies the CPU numerical reference"
    );
    expect(
        image::dcp_color_execution_backend_identity(image::DcpColorExecutionBackend::metal)
            == "dcp-executor=metal-v1;math=f32",
        "DCP cache identity identifies the Metal executor"
    );
    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        return;
    }

    auto definition = profile_definition(true);
    definition.profile.calibration1.hue_sat_map = value_scale_table(0.82F);
    definition.profile.look_table = value_scale_table(0.73F);
    definition.profile.tone_curve = {
        image::DcpToneCurvePoint{0.0F, 0.0F},
        image::DcpToneCurvePoint{0.5F, 0.31F},
        image::DcpToneCurvePoint{1.0F, 1.0F},
    };
    const auto transform = image::compile_dcp_color_transform(definition, raw_descriptor());
    image::SceneLinearRgbFrame cpu{
        .dimensions = image::Dimensions{5U, 1U},
        .row_stride_bytes = 5U * 3U * sizeof(float),
        .samples = {
            0.91F, 0.37F, 0.08F,
            0.18F, 0.63F, 0.92F,
            1.50F, 0.70F, 0.30F,
            -0.12F, 0.30F, 0.70F,
            1.80F, 1.10F, 0.40F,
        },
    };
    auto metal = cpu;
    {
        ScopedEnvironment backend("SHADOW_IMAGE_ACCELERATION", "cpu");
        expect(
            image::apply_dcp_color_rendering_stages(cpu, transform)
                == image::DcpColorExecutionBackend::cpu,
            "DCP CPU mode stays on the reference executor"
        );
    }
    {
        ScopedEnvironment backend("SHADOW_IMAGE_ACCELERATION", "metal");
        expect(
            image::apply_dcp_color_rendering_stages(metal, transform)
                == image::DcpColorExecutionBackend::metal,
            "DCP Metal mode dispatches the scene-linear GPU executor"
        );
    }
    expect(cpu.valid() && metal.valid(), "CPU and Metal DCP frames retain their contracts");
    expect(cpu.samples.size() == metal.samples.size(), "CPU and Metal DCP output shapes agree");
    for (std::size_t index = 0U; index < cpu.samples.size(); ++index) {
        expect_close(
            metal.samples[index],
            cpu.samples[index],
            2.5e-4,
            "Metal DCP agrees with the CPU reference within fp32 tolerance"
        );
    }
}

} // namespace

int main() {
    standard_dcp_rendering_stages_compile_and_apply();
    scene_linear_dcp_stages_preserve_highlight_headroom();
    large_scene_linear_dcp_stage_matches_the_single_pixel_reference();
    scene_linear_dcp_metal_matches_the_cpu_reference_when_available();
    std::cout << "shadow image DCP color rendering contract tests passed\n";
}
