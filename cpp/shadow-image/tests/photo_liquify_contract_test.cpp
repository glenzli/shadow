#include "edit_contract_test_support.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/photo_liquify.hpp>

#include <cmath>
#include <cstdlib>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

[[nodiscard]] image::PhotoLiquify horizontal_push() {
    return image::PhotoLiquify{
        .strokes =
            {
                image::PhotoLiquifyPushStroke{
                    .points =
                        {
                            {.x = 0.3, .y = 0.5, .pressure = 1.0},
                            {.x = 0.7, .y = 0.5, .pressure = 1.0},
                        },
                    .radius = 0.25,
                    .strength = 1.0,
                    .hardness = 0.5,
                },
            },
    };
}

[[nodiscard]] image::FloatRgbImage horizontal_gradient() {
    std::vector<float> samples;
    samples.reserve(9U * 9U * 3U);
    for (std::uint32_t y = 0U; y < 9U; ++y) {
        for (std::uint32_t x = 0U; x < 9U; ++x) {
            const float value = static_cast<float>(x) / 8.0F;
            samples.insert(samples.end(), {value, value, value});
        }
    }
    return rgb_raster(9U, 9U, std::move(samples));
}

[[nodiscard]] image::PhotoLiquify push_then_reconstruct() {
    auto result = horizontal_push();
    result.strokes.push_back(image::PhotoLiquifyReconstructStroke{
        .points =
            {
                {.x = 0.5, .y = 0.5, .pressure = 1.0},
            },
        .radius = 0.25,
        .strength = 1.0,
        .hardness = 1.0,
    });
    return result;
}

void preparation_is_resolution_specific_deterministic_and_bounded() {
    const auto prepared = image::prepare_photo_liquify({9U, 9U}, horizontal_push());
    expect(prepared.valid(), "prepared push gesture is executable");
    expect(prepared.source_dimensions == image::Dimensions{9U, 9U}, "plan binds source size");
    expect(prepared.stamps.size() == 7U, "quarter-radius spacing deterministically emits stamps");
    expect_close_double(
        prepared.stamps.front().radius,
        2.25,
        1.0e-12,
        "normalized radius lowers against the shorter source edge"
    );
    expect_close_double(
        prepared.maximum_displacement_pixels,
        3.6,
        1.0e-12,
        "plan exposes a conservative displacement preimage bound"
    );
    const auto repeated = image::prepare_photo_liquify({9U, 9U}, horizontal_push());
    expect(repeated == prepared, "the same authoring input and dimensions produce the same plan");
}

void inverse_composition_moves_content_with_one_final_sample() {
    const auto input = horizontal_gradient();
    const auto prepared = image::prepare_photo_liquify(input.dimensions, horizontal_push());
    const auto output = image::apply_photo_liquify(input, prepared);

    expect(output.dimensions == input.dimensions, "liquify preserves the uncropped canvas");
    expect(output.working_space == input.working_space, "liquify preserves working RGB metadata");
    const std::size_t moved_index = (4U * 9U + 5U) * 3U;
    expect(
        output.samples[moved_index] < input.samples[moved_index],
        "a rightward push inverse-samples content from the left"
    );
    expect_close(
        output.samples[0],
        input.samples[0],
        "pixels outside every brush support remain bit-close to their source"
    );
    expect_close(
        output.samples[1],
        output.samples[0],
        "one composed RGB sample keeps neutral channels paired"
    );
}

void reconstruct_blends_the_accumulated_mapping_toward_identity() {
    const auto input = horizontal_gradient();
    const auto push = image::apply_photo_liquify(
        input,
        image::prepare_photo_liquify(input.dimensions, horizontal_push())
    );
    const auto reconstructed_plan =
        image::prepare_photo_liquify(input.dimensions, push_then_reconstruct());
    const auto reconstructed = image::apply_photo_liquify(input, reconstructed_plan);
    const std::size_t center = (4U * 9U + 4U) * 3U;

    expect(
        std::abs(push.samples[center] - input.samples[center]) > 1.0e-4F,
        "the Push fixture deforms the reconstruction target"
    );
    expect_close(
        reconstructed.samples[center],
        input.samples[center],
        "a full-strength Reconstruct stamp restores the original mapping at its centre"
    );
    expect(
        reconstructed_plan.stamps.back().kind
            == image::PreparedPhotoLiquifyStampKind::reconstruct,
        "prepared operations retain the authored Reconstruct kind"
    );
}

void malformed_and_resolution_mismatched_inputs_fail_closed() {
    bool rejected_empty = false;
    try {
        image::validate_photo_liquify({});
    } catch (const image::DecodeError& error) {
        rejected_empty = error.code() == image::DecodeErrorCode::invalid_request;
    }
    expect(rejected_empty, "empty persisted Liquify nodes are rejected");

    bool rejected_reconstruct_first = false;
    try {
        image::validate_photo_liquify(image::PhotoLiquify{
            .strokes = {
                image::PhotoLiquifyReconstructStroke{
                    .points = {{.x = 0.5, .y = 0.5, .pressure = 1.0}},
                    .radius = 0.1,
                    .strength = 0.5,
                    .hardness = 0.5,
                },
            },
        });
    } catch (const image::DecodeError& error) {
        rejected_reconstruct_first =
            error.code() == image::DecodeErrorCode::invalid_request;
    }
    expect(
        rejected_reconstruct_first,
        "Reconstruct cannot exist before any deformation in the ordered node"
    );

    auto duplicate_points = horizontal_push();
    auto& push = std::get<image::PhotoLiquifyPushStroke>(duplicate_points.strokes[0]);
    push.points[1] = push.points[0];
    bool rejected_noop = false;
    try {
        static_cast<void>(image::prepare_photo_liquify({9U, 9U}, duplicate_points));
    } catch (const image::DecodeError& error) {
        rejected_noop = error.code() == image::DecodeErrorCode::invalid_request;
    }
    expect(rejected_noop, "a path with no effective displacement is rejected");

    const auto prepared = image::prepare_photo_liquify({9U, 9U}, horizontal_push());
    auto understated = prepared;
    understated.maximum_displacement_pixels *= 0.5;
    expect(!understated.valid(), "a prepared plan cannot understate its tile preimage bound");
    bool rejected_mismatch = false;
    try {
        static_cast<void>(
            image::apply_photo_liquify(
                rgb_raster(8U, 8U, std::vector<float>(8U * 8U * 3U, 0.25F)),
                prepared
            )
        );
    } catch (const image::DecodeError& error) {
        rejected_mismatch = error.code() == image::DecodeErrorCode::invalid_request;
    }
    expect(rejected_mismatch, "prepared plans cannot execute against another resolution");
}

} // namespace

int main() {
    preparation_is_resolution_specific_deterministic_and_bounded();
    inverse_composition_moves_content_with_one_final_sample();
    reconstruct_blends_the_accumulated_mapping_toward_identity();
    malformed_and_resolution_mismatched_inputs_fail_closed();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
