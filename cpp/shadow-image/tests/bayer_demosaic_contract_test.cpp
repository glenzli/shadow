#include "../src/raw/bayer_sampling.hpp"
#include "contract_test_assertions.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_development.hpp>
#include <shadow/image/raw_frame.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include <cstdlib>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

void bayer_bilinear_demosaic_keeps_the_sensor_domain_explicit() {
    image::RawFrame frame;
    frame.descriptor.schema_version = image::raw_frame_schema_version;
    frame.descriptor.storage_dimensions = {4U, 4U};
    frame.descriptor.active_dimensions = {4U, 4U};
    frame.descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    frame.descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    frame.descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    frame.descriptor.cfa_pattern = "RGGB";
    frame.descriptor.bits_per_sample = 12U;
    frame.descriptor.black_levels = {100U, 100U, 100U, 100U};
    frame.descriptor.white_levels = {1'100U, 1'100U, 1'100U, 1'100U};
    frame.descriptor.as_shot_neutral = {0.5, 1.0, 1.0, 0.75};
    frame.samples.resize(16U);
    for (std::uint32_t y = 0U; y < 4U; ++y) {
        for (std::uint32_t x = 0U; x < 4U; ++x) {
            const auto color = frame.descriptor.bayer_2x2[(y & 1U) * 2U + (x & 1U)];
            frame.samples[static_cast<std::size_t>(y) * 4U + x] =
                color == image::RawCfaColor::red     ? 300U
                : color == image::RawCfaColor::green ? 500U
                                                     : 900U;
        }
    }
    expect(frame.valid(), "constant Bayer fixture is a valid unprocessed RAW frame");

    const auto output = image::demosaic_bayer_bilinear(frame);
    expect(output.valid(), "bilinear Bayer demosaic produces a valid camera-linear RGB frame");
    expect(
        output.receipt.algorithm == image::RawDemosaicAlgorithm::bayer_bilinear_v1
            && output.receipt.black_subtraction_applied
            && output.receipt.white_level_normalization_applied
            && !output.receipt.white_balance_applied && !output.receipt.dng_opcodes_applied,
        "Bayer demosaic receipt never overclaims white balance or DNG opcode "
        "application"
    );
    for (std::size_t pixel = 0U; pixel < 16U; ++pixel) {
        const auto index = pixel * 3U;
        expect(
            std::abs(output.samples[index] - 0.2F) < 1.0e-6F
                && std::abs(output.samples[index + 1U] - 0.4F) < 1.0e-6F
                && std::abs(output.samples[index + 2U] - 0.8F) < 1.0e-6F,
            "bilinear Bayer reconstruction preserves per-CFA black/white "
            "normalized camera RGB"
        );
    }

    auto non_bayer = frame;
    non_bayer.descriptor.cfa_layout = image::RawFrameCfaLayout::unknown;
    try {
        static_cast<void>(image::demosaic_bayer_bilinear(non_bayer));
        expect(false, "Bayer demosaic must reject an unknown CFA layout");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported_layout,
            "unknown CFA layout is rejected before Bayer processing"
        );
    }
}

void edge_aware_demosaic_drops_directional_phase_at_a_saturated_frontier() {
    image::RawFrame frame;
    frame.descriptor.schema_version = image::raw_frame_schema_version;
    frame.descriptor.storage_dimensions = {12U, 12U};
    frame.descriptor.active_dimensions = {12U, 12U};
    frame.descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    frame.descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    frame.descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    frame.descriptor.cfa_pattern = "RGGB";
    frame.descriptor.bits_per_sample = 12U;
    frame.descriptor.black_levels = {100U, 100U, 100U, 100U};
    frame.descriptor.white_levels = {1'100U, 1'100U, 1'100U, 1'100U};
    frame.descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
    frame.samples.resize(12U * 12U);
    for (std::uint32_t y = 0U; y < 12U; ++y) {
        for (std::uint32_t x = 0U; x < 12U; ++x) {
            const bool clipped_side = 2U * x + y >= 17U;
            frame.samples[static_cast<std::size_t>(y) * 12U + x] = clipped_side ? 1'100U : 180U;
        }
    }
    expect(frame.valid(), "slanted saturated-edge fixture is a valid Bayer RAW frame");

    image::RawFrameLinearTransform transform;
    transform.apply_cfa_white_balance = true;
    transform.cfa_white_balance = {1.0, 1.0, 1.0, 1.0};
    const auto treatment = image::detail::editable_raw_cfa_sampling_policy(transform);
    const image::detail::BayerCfaSamplingPolicy untreated{};
    float treated_difference = 0.0F;
    float untreated_difference = 0.0F;
    for (std::uint32_t y = 3U; y < 9U; ++y) {
        for (std::uint32_t x = 3U; x < 9U; ++x) {
            if (2U * x + y < 17U || 2U * x + y > 22U) {
                continue;
            }
            const auto bilinear =
                image::detail::bilinear_camera_rgb_sample_at(frame, x, y, &transform, treatment);
            const auto treated =
                image::detail::edge_aware_camera_rgb_sample_at(frame, x, y, &transform, treatment);
            const auto directional =
                image::detail::edge_aware_camera_rgb_sample_at(frame, x, y, &transform, untreated);
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                treated_difference += std::abs(treated.values[channel] - bilinear.values[channel]);
                untreated_difference +=
                    std::abs(directional.values[channel] - bilinear.values[channel]);
            }
        }
    }
    expect(
        treated_difference + 0.25F < untreated_difference,
        "a saturated slanted frontier drops unreliable directional Bayer phase toward the existing "
        "bilinear estimate"
    );
}

} // namespace

int main() {
    bayer_bilinear_demosaic_keeps_the_sensor_domain_explicit();
    edge_aware_demosaic_drops_directional_phase_at_a_saturated_frontier();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
