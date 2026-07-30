#include "../src/raw/neural_raw_denoise/neural_raw_denoise.hpp"
#include "../src/concurrency/row_scheduler.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
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

[[nodiscard]] image::RawFrame calibrated_odd_bayer_frame() {
    image::RawFrame frame;
    auto& descriptor = frame.descriptor;
    descriptor.provider_id = "neural-raw-test-provider";
    descriptor.provider_version = "v1";
    descriptor.storage_dimensions = {9U, 7U};
    descriptor.active_dimensions = {7U, 5U};
    descriptor.active_margins = {
        .left = 1U,
        .top = 1U,
        .right = 1U,
        .bottom = 1U,
    };
    descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        image::RawCfaColor::blue,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::red,
    };
    descriptor.cfa_pattern = "BGGR";
    descriptor.bits_per_sample = 14U;
    descriptor.black_levels = {64U, 128U, 192U, 256U};
    descriptor.white_levels = {1'024U, 2'048U, 3'072U, 4'096U};
    descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
    descriptor.sensor_noise = image::RawSensorNoiseCalibration{
        .schema_version = image::raw_sensor_noise_calibration_schema_version,
        .model = image::RawSensorNoiseModel::poisson_gaussian_per_cfa,
        .source = image::RawSensorNoiseCalibrationSource::embedded_metadata,
        .iso_sensitivity = 3'200.0,
        .read_noise_stddev_dn = {1.0, 2.0, 3.0, 4.0},
        .shot_noise_variance_per_dn = {0.1, 0.2, 0.3, 0.4},
    };
    frame.samples.assign(
        static_cast<std::size_t>(descriptor.storage_dimensions.width)
            * descriptor.storage_dimensions.height,
        17U
    );
    for (std::uint32_t relative_y = 0U;
         relative_y < descriptor.active_dimensions.height;
         ++relative_y) {
        const std::uint32_t raw_y = descriptor.active_margins.top + relative_y;
        for (std::uint32_t relative_x = 0U;
             relative_x < descriptor.active_dimensions.width;
             ++relative_x) {
            const std::uint32_t raw_x = descriptor.active_margins.left + relative_x;
            const std::size_t site =
                static_cast<std::size_t>((raw_y & 1U) * 2U + (raw_x & 1U));
            frame.samples[
                static_cast<std::size_t>(raw_y) * descriptor.storage_dimensions.width + raw_x
            ] = static_cast<std::uint16_t>(
                descriptor.black_levels[site] + 100U * (site + 1U) + relative_y * 7U
                + relative_x
            );
        }
    }
    return frame;
}

[[nodiscard]] image::detail::NeuralRawDenoiseConfiguration valid_configuration() {
    return image::detail::NeuralRawDenoiseConfiguration{
        .enabled = true,
        .apply_to_preview = false,
        .compiled_model_path = "/nonexistent/shadow-neural-raw.mlmodelc",
        .expected_model_content_identity = "sha256-tree-v1:"
                                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .packed_tile_edge = 4U,
        .packed_halo = 1U,
    };
}

class IdentityInference final : public image::detail::NeuralRawTileInference {
  public:
    void infer(
        const std::span<const float> packed_mosaic,
        const std::span<const float> noise,
        const std::uint32_t packed_tile_edge,
        const std::span<float> denoised_packed_mosaic
    ) override {
        ++calls;
        if (calls == 1U) {
            first_mosaic.assign(packed_mosaic.begin(), packed_mosaic.end());
            first_noise.assign(noise.begin(), noise.end());
            edge = packed_tile_edge;
        }
        std::copy(
            packed_mosaic.begin(),
            packed_mosaic.end(),
            denoised_packed_mosaic.begin()
        );
    }

    std::size_t calls = 0U;
    std::uint32_t edge = 0U;
    std::vector<float> first_mosaic;
    std::vector<float> first_noise;
};

[[nodiscard]] float normalized_at(
    const image::RawFrame& frame,
    const std::uint32_t raw_x,
    const std::uint32_t raw_y
) {
    const auto& descriptor = frame.descriptor;
    const std::size_t site =
        static_cast<std::size_t>((raw_y & 1U) * 2U + (raw_x & 1U));
    const std::uint16_t sample =
        frame.samples[
            static_cast<std::size_t>(raw_y) * descriptor.storage_dimensions.width + raw_x
        ];
    return static_cast<float>(
        static_cast<double>(sample - descriptor.black_levels[site])
        / static_cast<double>(
            descriptor.white_levels[site] - descriptor.black_levels[site]
        )
    );
}

void canonical_packing_tiling_and_odd_edges_are_stable() {
    const image::RawFrame source = calibrated_odd_bayer_frame();
    IdentityInference inference;
    const image::RawFrame result =
        image::detail::execute_neural_raw_denoise_tiles(source, 4U, 1U, inference);

    expect(
        inference.calls == 4U && inference.edge == 4U,
        "packed 4x3 Bayer data is covered by four haloed 4x4 tiles"
    );
    expect(
        result.samples == source.samples,
        "identity inference round-trips odd active dimensions and preserves sensor margins"
    );
    const std::size_t plane = 16U;
    const std::size_t tile_center = 5U;
    // BGGR at raw origin with an odd/odd active origin canonicalizes to:
    // R=(1,1), Gr=(2,1), Gb=(1,2), B=(2,2).
    expect(
        inference.first_mosaic.size() == plane * 4U
            && std::abs(
                   inference.first_mosaic[0U * plane + tile_center]
                   - normalized_at(source, 1U, 1U)
               )
                   < 1.0e-6F
            && std::abs(
                   inference.first_mosaic[1U * plane + tile_center]
                   - normalized_at(source, 2U, 1U)
               )
                   < 1.0e-6F
            && std::abs(
                   inference.first_mosaic[2U * plane + tile_center]
                   - normalized_at(source, 1U, 2U)
               )
                   < 1.0e-6F
            && std::abs(
                   inference.first_mosaic[3U * plane + tile_center]
                   - normalized_at(source, 2U, 2U)
               )
                   < 1.0e-6F,
        "model input is canonical R/Gr/Gb/B regardless of source Bayer phase and active margins"
    );

    const std::array<std::size_t, 4U> canonical_sites{3U, 2U, 1U, 0U};
    bool noise_matches = inference.first_noise.size() == 8U;
    for (std::size_t channel = 0U; channel < canonical_sites.size() && noise_matches; ++channel) {
        const std::size_t site = canonical_sites[channel];
        const double range = static_cast<double>(
            source.descriptor.white_levels[site] - source.descriptor.black_levels[site]
        );
        noise_matches =
            std::abs(
                inference.first_noise[channel]
                - static_cast<float>(
                    source.descriptor.sensor_noise.read_noise_stddev_dn[site] / range
                )
            )
                < 1.0e-7F
            && std::abs(
                   inference.first_noise[channel + 4U]
                   - static_cast<float>(
                       source.descriptor.sensor_noise.shot_noise_variance_per_dn[site] / range
                   )
               )
                   < 1.0e-7F;
    }
    expect(
        noise_matches,
        "noise conditioning carries normalized per-CFA read and shot parameters"
    );
}

class FailingInference final : public image::detail::NeuralRawTileInference {
  public:
    void infer(
        const std::span<const float> packed_mosaic,
        const std::span<const float>,
        const std::uint32_t,
        const std::span<float> output
    ) override {
        ++calls;
        if (calls == 2U) {
            throw std::runtime_error("synthetic inference failure");
        }
        std::copy(packed_mosaic.begin(), packed_mosaic.end(), output.begin());
    }

    std::size_t calls = 0U;
};

class IncompleteInference final : public image::detail::NeuralRawTileInference {
  public:
    void infer(
        std::span<const float>,
        std::span<const float>,
        std::uint32_t,
        std::span<float>
    ) override {}
};

void tile_failure_cannot_mutate_the_source_transaction() {
    const image::RawFrame source = calibrated_odd_bayer_frame();
    const auto before = source.samples;
    FailingInference inference;
    bool failed = false;
    try {
        static_cast<void>(
            image::detail::execute_neural_raw_denoise_tiles(source, 4U, 1U, inference)
        );
    } catch (const std::runtime_error& error) {
        failed = std::string_view(error.what()) == "synthetic inference failure";
    }
    expect(
        failed && source.samples == before,
        "a later tile failure leaves the borrowed source plane byte-for-byte unchanged"
    );

    IncompleteInference incomplete;
    bool rejected_incomplete_output = false;
    try {
        static_cast<void>(
            image::detail::execute_neural_raw_denoise_tiles(source, 4U, 1U, incomplete)
        );
    } catch (const image::DecodeError& error) {
        rejected_incomplete_output =
            error.code() == image::DecodeErrorCode::internal;
    }
    expect(
        rejected_incomplete_output && source.samples == before,
        "an inference backend must initialize every finite output value before publication"
    );

    std::stop_source cancellation;
    cancellation.request_stop();
    const image::detail::ScopedRowCancellation scoped_cancellation(
        cancellation.get_token()
    );
    IdentityInference cancelled_inference;
    bool cancelled = false;
    try {
        static_cast<void>(image::detail::execute_neural_raw_denoise_tiles(
            source,
            4U,
            1U,
            cancelled_inference
        ));
    } catch (const image::detail::RowExecutionCancelled&) {
        cancelled = true;
    }
    expect(
        cancelled && cancelled_inference.calls == 0U && source.samples == before,
        "cancellation stops before the next tile and cannot publish a partial RAW plane"
    );
}

void preparation_is_explicit_preview_aware_and_calibration_gated() {
    const image::RawFrame source = calibrated_odd_bayer_frame();
    const auto configuration = valid_configuration();
    const auto ready =
        image::detail::prepare_neural_raw_denoise(source, configuration, false);
    expect(
        ready.requested() && ready.execution_requested(),
        "an explicit calibrated detail request prepares neural RAW execution"
    );

    const auto preview =
        image::detail::prepare_neural_raw_denoise(source, configuration, true);
    const auto preview_result =
        image::detail::execute_prepared_neural_raw_denoise(source, preview);
    expect(
        preview.readiness == image::detail::NeuralRawDenoiseReadiness::bypassed_preview
            && preview_result.receipt.valid() && !preview_result.receipt.applied()
            && preview_result.receipt.status
                   == image::detail::NeuralRawDenoiseStatus::bypassed_preview
            && preview_result.frame.samples == source.samples,
        "preview development bypasses the heavy node unless explicitly admitted"
    );

    auto uncalibrated = source;
    uncalibrated.descriptor.sensor_noise = image::RawSensorNoiseCalibration{};
    const auto unsupported =
        image::detail::prepare_neural_raw_denoise(uncalibrated, configuration, false);
    const auto unsupported_result =
        image::detail::execute_prepared_neural_raw_denoise(uncalibrated, unsupported);
    expect(
        unsupported.readiness == image::detail::NeuralRawDenoiseReadiness::unsupported_source
            && unsupported_result.receipt.valid()
            && unsupported_result.receipt.status
                   == image::detail::NeuralRawDenoiseStatus::fallback_unsupported_source
            && unsupported_result.frame.samples == uncalibrated.samples,
        "missing calibrated sensor noise is an explicit source fallback, never a guessed model"
    );

    auto invalid_configuration = configuration;
    invalid_configuration.expected_model_content_identity = "not-a-content-identity";
    const auto invalid =
        image::detail::prepare_neural_raw_denoise(source, invalid_configuration, false);
    const auto invalid_result =
        image::detail::execute_prepared_neural_raw_denoise(source, invalid);
    expect(
        invalid.readiness == image::detail::NeuralRawDenoiseReadiness::invalid_configuration
            && invalid_result.receipt.valid()
            && invalid_result.receipt.status
                   == image::detail::NeuralRawDenoiseStatus::fallback_invalid_configuration
            && invalid_result.frame.samples == source.samples,
        "an unpinned model configuration falls back explicitly without touching RAW samples"
    );
}

void runtime_failure_is_truthful_and_atomic() {
    const image::RawFrame source = calibrated_odd_bayer_frame();
    const auto configuration = valid_configuration();
    const auto prepared =
        image::detail::prepare_neural_raw_denoise(source, configuration, false);
    const auto result =
        image::detail::execute_prepared_neural_raw_denoise(source, prepared);
    expect(
        result.receipt.valid() && !result.receipt.applied()
            && result.receipt.status
                   == image::detail::NeuralRawDenoiseStatus::fallback_runtime_failure
            && !result.receipt.diagnostic.empty() && result.frame.samples == source.samples
            && result.receipt.cache_identity.find(
                   configuration.expected_model_content_identity
               )
                   != std::string::npos,
        "missing Core ML artifacts retain the source and publish an exact requested-model fallback"
    );

    const auto disabled = image::detail::prepare_neural_raw_denoise(
        source,
        image::detail::NeuralRawDenoiseConfiguration{},
        false
    );
    const auto disabled_result =
        image::detail::execute_prepared_neural_raw_denoise(source, disabled);
    expect(
        disabled_result.receipt.valid()
            && disabled_result.receipt.status
                   == image::detail::NeuralRawDenoiseStatus::disabled
            && disabled_result.receipt.cache_identity.find(
                   "neural-raw-denoise-model=none"
               )
                   != std::string::npos,
        "the absent node remains distinguishable from requested execution and fallback"
    );
}

[[nodiscard]] std::uint32_t required_test_u32(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        expect(false, std::string(name) + " is required by the Core ML fixture gate");
        return 0U;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > 0xffff'ffffUL) {
        expect(false, std::string(name) + " must be an unsigned 32-bit integer");
        return 0U;
    }
    return static_cast<std::uint32_t>(parsed);
}

void optional_real_coreml_checkpoint_gate() {
    const char* path = std::getenv("SHADOW_TEST_NEURAL_RAW_DENOISE_MODEL");
    const char* identity =
        std::getenv("SHADOW_TEST_NEURAL_RAW_DENOISE_MODEL_IDENTITY");
    const char* tile = std::getenv("SHADOW_TEST_NEURAL_RAW_DENOISE_TILE_EDGE");
    const char* halo = std::getenv("SHADOW_TEST_NEURAL_RAW_DENOISE_HALO");
    if (path == nullptr && identity == nullptr && tile == nullptr && halo == nullptr) {
        std::cout
            << "INFO: real Core ML checkpoint gate not requested; portable contracts ran\n";
        return;
    }
    if (path == nullptr || identity == nullptr) {
        expect(
            false,
            "real Core ML checkpoint gate requires both model path and tree identity"
        );
        return;
    }
    const image::RawFrame source = calibrated_odd_bayer_frame();
    const image::detail::NeuralRawDenoiseConfiguration configuration{
        .enabled = true,
        .apply_to_preview = true,
        .compiled_model_path = path,
        .expected_model_content_identity = identity,
        .packed_tile_edge =
            required_test_u32("SHADOW_TEST_NEURAL_RAW_DENOISE_TILE_EDGE"),
        .packed_halo = required_test_u32("SHADOW_TEST_NEURAL_RAW_DENOISE_HALO"),
    };
    const auto prepared =
        image::detail::prepare_neural_raw_denoise(source, configuration, false);
    const auto result =
        image::detail::execute_prepared_neural_raw_denoise(source, prepared);
    expect(
        prepared.execution_requested() && result.receipt.valid() && result.receipt.applied()
            && result.receipt.model_content_identity
                   == configuration.expected_model_content_identity
            && result.frame.valid()
            && result.frame.descriptor.storage_dimensions
                   == source.descriptor.storage_dimensions,
        "a supplied checkpoint executes the real Core ML adapter with exact provenance"
    );
}

} // namespace

int main() {
    canonical_packing_tiling_and_odd_edges_are_stable();
    tile_failure_cannot_mutate_the_source_transaction();
    preparation_is_explicit_preview_aware_and_calibration_gated();
    runtime_failure_is_truthful_and_atomic();
    optional_real_coreml_checkpoint_gate();
    return failures == 0 ? 0 : 1;
}
