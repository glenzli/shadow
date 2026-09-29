#include "../src/raw/bayer_sampling.hpp"
#include "contract_test_assertions.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_development.hpp>
#include <shadow/image/raw_frame.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

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

void opposed_reconstruction_repairs_the_terminal_cfa_site_before_demosaic() {
    image::RawFrame frame;
    frame.descriptor.schema_version = image::raw_frame_schema_version;
    frame.descriptor.storage_dimensions = {7U, 7U};
    frame.descriptor.active_dimensions = {7U, 7U};
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
    frame.descriptor.black_levels = {0U, 0U, 0U, 0U};
    frame.descriptor.white_levels = {1'000U, 1'000U, 1'000U, 1'000U};
    frame.descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
    frame.samples.resize(49U);
    for (std::uint32_t y = 0U; y < 7U; ++y) {
        for (std::uint32_t x = 0U; x < 7U; ++x) {
            const auto color = frame.descriptor.bayer_2x2[(y & 1U) * 2U + (x & 1U)];
            frame.samples[static_cast<std::size_t>(y) * 7U + x] =
                color == image::RawCfaColor::red ? 950U : 900U;
        }
    }
    frame.samples[2U * 7U + 2U] = 1'000U;
    frame.samples[4U * 7U + 4U] = 986U;
    expect(frame.valid(), "terminal opposed fixture is a valid Bayer RAW frame");

    image::RawFrameLinearTransform transform;
    transform.apply_cfa_white_balance = true;
    transform.cfa_white_balance = {1.0, 2.0, 2.0, 2.0};
    const auto treatment = image::detail::editable_raw_cfa_sampling_policy(transform);
    const auto clipped =
        image::detail::opposed_highlight_cfa_sample_at(frame, 2U, 2U, &transform, treatment);
    expect(
        clipped.terminal_candidate && std::abs(clipped.measured - 1.0F) < 1.0e-6F
            && clipped.opposed_reference > 1.79F && clipped.opposed_reference < 1.81F
            && std::abs(clipped.reconstructed - clipped.opposed_reference) < 1.0e-6F,
        "a terminal CFA photosite is reconstructed upward from the two local opposed colours "
        "before "
        "demosaic"
    );

    auto headroom_frame = frame;
    headroom_frame.samples[2U * 7U + 3U] = 1'000U;
    const auto isolated_terminal = image::detail::opposed_highlight_cfa_sample_at(
        headroom_frame,
        3U,
        2U,
        &transform,
        treatment
    );
    expect(
        std::abs(isolated_terminal.measured - 2.0F) < 1.0e-6F,
        "production retains white-balanced fp32 headroom for an isolated terminal CFA phase"
    );

    auto shared_only_diagnostic = treatment;
    shared_only_diagnostic.require_shared_terminal_headroom = true;
    const auto projected_isolated_terminal = image::detail::opposed_highlight_cfa_sample_at(
        headroom_frame,
        3U,
        2U,
        &transform,
        shared_only_diagnostic
    );
    expect(
        std::abs(projected_isolated_terminal.measured - 1.0F) < 1.0e-6F,
        "the shared-only diagnostic can still project an isolated terminal phase to common white"
    );

    auto shared_headroom_frame = headroom_frame;
    for (std::uint32_t y = 1U; y <= 3U; ++y) {
        for (std::uint32_t x = 2U; x <= 4U; ++x) {
            shared_headroom_frame.samples[static_cast<std::size_t>(y) * 7U + x] = 1'000U;
        }
    }
    const auto scene_referred = treatment;
    const auto retained_headroom = image::detail::opposed_highlight_cfa_sample_at(
        shared_headroom_frame,
        3U,
        2U,
        &transform,
        scene_referred
    );
    expect(
        retained_headroom.terminal_candidate
            && std::abs(retained_headroom.measured - 2.0F) < 1.0e-6F
            && std::abs(retained_headroom.reconstructed - retained_headroom.measured) < 1.0e-6F,
        "the production scene-referred domain retains CFA white-balance headroom in a shared "
        "physical-white core"
    );

    auto common_white_diagnostic = treatment;
    common_white_diagnostic.preserve_terminal_white_balance_headroom = false;
    const auto projected_common_white = image::detail::opposed_highlight_cfa_sample_at(
        shared_headroom_frame,
        3U,
        2U,
        &transform,
        common_white_diagnostic
    );
    expect(
        std::abs(projected_common_white.measured - 1.0F) < 1.0e-6F,
        "the old common-white projection remains available only to bounded diagnostics"
    );

    const auto below_threshold =
        image::detail::opposed_highlight_cfa_sample_at(frame, 4U, 4U, &transform, treatment);
    expect(
        !below_threshold.terminal_candidate
            && std::abs(below_threshold.reconstructed - below_threshold.measured) < 1.0e-6F,
        "the darktable-compatible 98.7 percent gate leaves measured CFA response below the "
        "terminal "
        "frontier unchanged"
    );

    auto response_limited = frame;
    response_limited.descriptor.has_linear_response_limits = true;
    response_limited.descriptor.linear_response_limits = {900U, 900U, 900U, 900U};
    response_limited.samples[4U * 7U + 4U] = 889U;
    const auto production_physical_white = image::detail::opposed_highlight_cfa_sample_at(
        response_limited,
        4U,
        4U,
        &transform,
        treatment
    );
    expect(
        !production_physical_white.terminal_candidate
            && std::abs(
                   production_physical_white.reconstructed - production_physical_white.measured
               ) < 1.0e-6F,
        "production excludes a response-terminal photosite that has not reached physical white"
    );
    auto response_limit_diagnostic = treatment;
    response_limit_diagnostic.terminal_highlight_admission =
        image::detail::CfaTerminalHighlightAdmission::linear_response_limit;
    const auto response_terminal = image::detail::opposed_highlight_cfa_sample_at(
        response_limited,
        4U,
        4U,
        &transform,
        response_limit_diagnostic
    );
    expect(
        response_terminal.terminal_candidate
            && response_terminal.reconstructed > response_terminal.measured + 0.1F,
        "the offline response-limit diagnostic retains the broader calibrated admission branch"
    );
    response_limited.samples[4U * 7U + 4U] = 1'000U;
    const auto physical_white_terminal = image::detail::opposed_highlight_cfa_sample_at(
        response_limited,
        4U,
        4U,
        &transform,
        treatment
    );
    expect(
        physical_white_terminal.terminal_candidate,
        "production admits a photosite at calibrated physical white"
    );
    response_limited.samples[4U * 7U + 4U] = 888U;
    const auto response_measured = image::detail::opposed_highlight_cfa_sample_at(
        response_limited,
        4U,
        4U,
        &transform,
        response_limit_diagnostic
    );
    expect(
        !response_measured.terminal_candidate
            && std::abs(response_measured.reconstructed - response_measured.measured) < 1.0e-6F,
        "the 98.7 percent gate remains strict in the calibrated linear-response domain"
    );

    auto baseline = treatment;
    baseline.reconstruct_terminal_highlights = false;
    const auto before =
        image::detail::bilinear_camera_rgb_sample_at(frame, 2U, 2U, &transform, baseline);
    const auto after =
        image::detail::bilinear_camera_rgb_sample_at(frame, 2U, 2U, &transform, treatment);
    expect(
        after.values[0] > before.values[0] + 0.19F
            && std::abs(after.values[1] - before.values[1]) < 1.0e-6F
            && std::abs(after.values[2] - before.values[2]) < 1.0e-6F,
        "demosaic aggregates the repaired source photosite instead of raising an "
        "already-aggregated "
        "RGB channel by a coverage fraction"
    );

    auto monochromatic = frame;
    for (std::uint32_t y = 1U; y <= 3U; ++y) {
        for (std::uint32_t x = 1U; x <= 3U; ++x) {
            const auto color = monochromatic.descriptor.bayer_2x2[(y & 1U) * 2U + (x & 1U)];
            if (color != image::RawCfaColor::red) {
                monochromatic.samples[static_cast<std::size_t>(y) * 7U + x] = 50U;
            }
        }
    }
    const auto saturated_red = image::detail::opposed_highlight_cfa_sample_at(
        monochromatic,
        2U,
        2U,
        &transform,
        treatment
    );
    expect(
        std::abs(saturated_red.reconstructed - saturated_red.measured) < 1.0e-6F,
        "one-sided opposed reconstruction preserves a genuinely saturated single-colour emitter"
    );
}

void area_opposed_reconstruction_preserves_reliable_mixed_pixel_contributions() {
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
    frame.descriptor.black_levels = {0U, 0U, 0U, 0U};
    frame.descriptor.white_levels = {1'000U, 1'000U, 1'000U, 1'000U};
    frame.descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
    frame.samples.resize(16U);
    for (std::uint32_t y = 0U; y < 4U; ++y) {
        for (std::uint32_t x = 0U; x < 4U; ++x) {
            // One output pixel deliberately straddles a reliable dark subject and a fully
            // terminal light. The two halves must remain separate contributions to its area
            // average even though they share the final RGB tuple.
            frame.samples[static_cast<std::size_t>(y) * 4U + x] = x < 2U ? 100U : 1'000U;
        }
    }
    expect(frame.valid(), "mixed-subject area fixture is a valid Bayer RAW frame");

    image::RawFrameLinearTransform transform;
    transform.apply_cfa_white_balance = true;
    transform.cfa_white_balance = {1.0, 2.0, 2.0, 2.0};
    const auto treatment = image::detail::editable_raw_cfa_sampling_policy(transform);
    const auto mixed_grid = image::detail::make_bayer_area_sampling_grid(frame, {1U, 1U});
    const auto split_grid = image::detail::make_bayer_area_sampling_grid(frame, {2U, 1U});
    const auto sample = image::detail::area_camera_rgb_sample_at(
        frame,
        mixed_grid,
        0U,
        0U,
        &transform,
        treatment
    );
    const auto dark = image::detail::area_camera_rgb_sample_at(
        frame,
        split_grid,
        0U,
        0U,
        &transform,
        treatment
    );
    const auto light = image::detail::area_camera_rgb_sample_at(
        frame,
        split_grid,
        1U,
        0U,
        &transform,
        treatment
    );
    auto untreated = treatment;
    untreated.cap_physical_sensor_white = false;
    untreated.reconstruct_terminal_highlights = false;
    const auto measured_light = image::detail::area_camera_rgb_sample_at(
        frame,
        split_grid,
        1U,
        0U,
        &transform,
        untreated
    );

    expect(
        std::abs(sample.highlight_channel_evidence[0U] - 0.5F) < 1.0e-6F
            && std::abs(sample.highlight_channel_evidence[1U] - 0.5F) < 1.0e-6F
            && std::abs(sample.highlight_channel_evidence[2U] - 0.5F) < 1.0e-6F,
        "area highlight evidence owns exactly the terminal half of every CFA channel"
    );
    bool owned_repair_matches =
        std::abs(dark.values[0U] - 0.1F) < 1.0e-6F
        && std::abs(dark.values[1U] - 0.2F) < 1.0e-6F
        && std::abs(dark.values[2U] - 0.2F) < 1.0e-6F;
    for (std::size_t channel = 0U; channel < sample.values.size(); ++channel) {
        owned_repair_matches =
            owned_repair_matches
            && std::abs(sample.values[channel] - 0.5F * (dark.values[channel] + light.values[channel]))
                   < 1.0e-5F;
    }
    bool light_was_only_raised = light.values[0U] > measured_light.values[0U] + 1.0e-3F;
    for (std::size_t channel = 0U; channel < light.values.size(); ++channel) {
        light_was_only_raised =
            light_was_only_raised
            && light.values[channel] >= measured_light.values[channel] - 1.0e-6F;
    }
    owned_repair_matches = owned_repair_matches && light_was_only_raised;
    if (!owned_repair_matches) {
        std::cerr << "area owned repair mixed/dark/light/measured-light="
                  << sample.values[0U] << ',' << sample.values[1U] << ',' << sample.values[2U]
                  << '/' << dark.values[0U] << ',' << dark.values[1U] << ',' << dark.values[2U]
                  << '/' << light.values[0U] << ',' << light.values[1U] << ',' << light.values[2U]
                  << '/' << measured_light.values[0U] << ',' << measured_light.values[1U] << ','
                  << measured_light.values[2U] << '\n';
    }
    expect(
        owned_repair_matches,
        "area highlight repair applies the point-owned terminal reconstruction before reduction, "
        "preserves the measured dark contribution, and leaves the mixed output as their exact "
        "area integral"
    );
}

} // namespace

int main() {
    bayer_bilinear_demosaic_keeps_the_sensor_domain_explicit();
    edge_aware_demosaic_drops_directional_phase_at_a_saturated_frontier();
    opposed_reconstruction_repairs_the_terminal_cfa_site_before_demosaic();
    area_opposed_reconstruction_preserves_reliable_mixed_pixel_contributions();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
