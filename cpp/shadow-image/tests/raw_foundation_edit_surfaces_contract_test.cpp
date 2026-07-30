#include "raw_pipeline_contract_test_support.hpp"

#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/raw_foundation.hpp>

#include <array>
#include <string>
#include <vector>

namespace {

constexpr std::string_view source_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view artifact_digest =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view cache_key_digest =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

[[nodiscard]] image::RawFoundationCameraRgbView
foundation(const std::vector<float>& pixels, const image::Dimensions dimensions = {4U, 4U}) {
    return image::RawFoundationCameraRgbView{
        .dimensions = dimensions,
        .samples = pixels,
        .provenance = {
            .source_sha256 = std::string(source_digest),
            .artifact_file_sha256 = std::string(artifact_digest),
            .cache_key_sha256 = std::string(cache_key_digest),
            .model_identity = std::string(image::raw_foundation_model_identity),
            .implementation_revision = std::string(image::raw_foundation_implementation_revision),
        },
    };
}

[[nodiscard]] std::vector<float> foundation_pixels() {
    std::vector<float> pixels(4U * 4U * 3U);
    for (std::size_t index = 0U; index < pixels.size(); index += 3U) {
        pixels[index] = 0.125F;
        pixels[index + 1U] = 0.25F;
        pixels[index + 2U] = 0.5F;
    }
    return pixels;
}

void warm_preview_retains_foundation_identity_and_renders() {
    const std::vector<float> pixels = foundation_pixels();
    SyntheticRawSession decoder(synthetic_bayer_frame());
    const auto warm = image::prepare_warm_edit_preview(
        decoder,
        2U,
        image::preview_raw_development_plan(),
        foundation(pixels)
    );
    expect(
        warm.dimensions() == image::Dimensions{.width = 2U, .height = 2U}
            && warm.raw_pipeline_receipt().pipeline_identity.find(artifact_digest)
                   != std::string::npos,
        "warm preview retains bounded foundation pixels and artifact identity"
    );
    expect(
        warm.raw_development_receipt().effective_plan.noise_reduction
                == image::RawNoiseReductionIntent::disabled
            && warm.sensor_clipping_mask().has_value()
            && warm.sensor_clipping_mask()->dimensions == warm.dimensions(),
        "warm preview retains adjusted RAW provenance and aligned sensor diagnostics"
    );
    const std::array<image::AdjustmentNode, 0U> neutral{};
    const auto rendered = warm.render_rgb8(neutral);
    expect(
        rendered.dimensions == warm.dimensions() && rendered.bytes.size() == 2U * 2U * 3U,
        "warm foundation session reaches the existing interactive render surface"
    );
    expect(
        decoder.raw_frame_count() == 1U && decoder.processed_count() == 0U,
        "warm foundation source never reopens provider RGB"
    );
}

void full_detail_materializes_foundation_and_renders_tiles() {
    const std::vector<float> pixels = foundation_pixels();
    SyntheticRawSession decoder(synthetic_bayer_frame());
    const auto detail = image::prepare_full_edit_detail(
        decoder,
        image::default_raw_development_plan(),
        foundation(pixels),
        image::FullEditDetailSourceRequirements{
            .requires_cpu_replay = true,
        }
    );
    expect(
        detail.dimensions() == image::Dimensions{.width = 4U, .height = 4U}
            && detail.retained_bytes() == 4U * 4U * 3U * sizeof(float)
            && detail.cpu_replay_available(),
        "full detail retains one materialized scene-linear foundation with CPU replay"
    );
    expect(
        detail.raw_pipeline_receipt().pipeline_identity.find("raw-foundation-developer")
                != std::string::npos
            && detail.raw_pipeline_receipt().pipeline_identity.find(artifact_digest)
                   != std::string::npos,
        "full detail retains the same foundation source identity"
    );
    const std::array<image::AdjustmentNode, 0U> neutral{};
    const auto rendered = detail.render_rgb8(
        neutral,
        image::DetailTileRect{
            .x = 1U,
            .y = 1U,
            .width = 2U,
            .height = 2U,
        }
    );
    expect(
        rendered.bytes.size() == 2U * 2U * 3U
            && rendered.full_dimensions == image::Dimensions{.width = 4U, .height = 4U},
        "materialized foundation reaches the existing full-detail tile renderer"
    );
    expect(
        decoder.raw_frame_count() == 1U && decoder.processed_count() == 0U,
        "detail foundation source decodes calibration once and never provider RGB"
    );
}

void edit_surfaces_reject_mismatched_artifacts_without_fallback() {
    const std::vector<float> wrong_pixels(2U * 4U * 3U, 0.25F);
    SyntheticRawSession warm_decoder(synthetic_bayer_frame());
    try {
        static_cast<void>(image::prepare_warm_edit_preview(
            warm_decoder,
            2U,
            image::preview_raw_development_plan(),
            foundation(wrong_pixels, image::Dimensions{.width = 2U, .height = 4U})
        ));
        expect(false, "warm surface must reject mismatched foundation geometry");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request
                && warm_decoder.processed_count() == 0U,
            "warm mismatch cannot fall back after an enabled foundation request"
        );
    }

    SyntheticRawSession detail_decoder(synthetic_bayer_frame());
    try {
        static_cast<void>(image::prepare_full_edit_detail(
            detail_decoder,
            image::default_raw_development_plan(),
            foundation(wrong_pixels, image::Dimensions{.width = 2U, .height = 4U})
        ));
        expect(false, "detail surface must reject mismatched foundation geometry");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request
                && detail_decoder.processed_count() == 0U,
            "detail mismatch cannot fall back after an enabled foundation request"
        );
    }
}

} // namespace

int main() {
    warm_preview_retains_foundation_identity_and_renders();
    full_detail_materializes_foundation_and_renders_tiles();
    edit_surfaces_reject_mismatched_artifacts_without_fallback();
    return failures == 0 ? 0 : 1;
}
