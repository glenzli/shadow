#include "contract_test_assertions.hpp"
#include "optics_test_fixture.hpp"
#include "processed_rgb_session_fixture.hpp"

#include <shadow/image/decoder.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/edit.hpp>
#include <shadow/image/optics.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::FakeRgbSession;
using shadow::image::test_support::processed_linear_gradient;
using shadow::image::test_support::ReplacingOpticsProvider;

void optics_settings_are_explicit_and_provider_safe() {
  const auto defaults = image::default_optics_settings();
  expect(defaults.schema_version == image::optics_settings_schema_version,
         "optics defaults declare the current schema");
  expect(defaults.enabled && defaults.correct_distortion &&
             defaults.correct_tca && defaults.correct_vignetting &&
             defaults.automatic_scale,
         "optics defaults preserve all automatic profile corrections");
  const auto default_signature = image::optics_settings_signature(defaults);
  auto no_tca = defaults;
  no_tca.correct_tca = false;
  expect(image::optics_settings_signature(no_tca) != default_signature,
         "every optics choice participates in cache identity");

  const auto provider = image::make_lensfun_optics_provider();
  auto disabled = defaults;
  disabled.enabled = false;
  const auto disabled_result = provider->correct_reference_rgb(
      processed_linear_gradient(), image::AssetMetadata{}, disabled);
  expect(disabled_result.receipt.status == image::OpticsProfileStatus::disabled,
         "disabled optics do not require profile metadata");
  expect(!disabled_result.corrected_reference_rgb.has_value(),
         "disabled optics do not duplicate the reference raster");

  auto unsupported_schema = defaults;
  unsupported_schema.schema_version += 1U;
  try {
    static_cast<void>(image::optics_settings_signature(unsupported_schema));
    expect(false, "unknown optics settings schemas must fail closed");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::invalid_request,
           "unknown optics schema reports invalid request");
  }
}

void scene_linear_optics_preserves_headroom_for_manual_correction() {
  image::SceneLinearRgbFrame input;
  input.dimensions = {4U, 4U};
  input.row_stride_bytes = 4U * 3U * sizeof(float);
  input.samples.assign(4U * 4U * 3U, 1.5F);
  expect(input.valid(), "scene-linear optics fixture has a valid fp32 layout");

  auto settings = image::default_optics_settings();
  settings.correct_distortion = false;
  settings.correct_tca = false;
  settings.correct_vignetting = false;
  settings.manual_vignetting_amount = 100;
  settings.manual_vignetting_midpoint = 0U;
  const auto provider = image::make_lensfun_optics_provider();
  const auto result = provider->correct_scene_linear_reference(
      input, image::AssetMetadata{}, settings);
  expect(result.receipt.status !=
             image::OpticsProfileStatus::incompatible_input,
         "Lensfun's float optics path does not reject an owned scene-linear "
         "RAW frame");
  expect(result.corrected_scene_linear_rgb.has_value(),
         "manual scene-linear optics materializes a corrected float frame");
  if (!result.corrected_scene_linear_rgb.has_value()) {
    return;
  }
  const auto &output = *result.corrected_scene_linear_rgb;
  expect(output.valid(),
         "manual scene-linear optics keeps its fp32 frame valid");
  expect(output.samples.front() > input.samples.front() &&
             output.samples.front() > 1.0F,
         "manual optical vignetting preserves and increases super-white RAW "
         "headroom");
}

void lensfun_adapter_applies_a_real_profile_when_a_test_database_is_available() {
  const auto *database = std::getenv("SHADOW_TEST_LENSFUN_DB");
  if (database == nullptr || *database == '\0') {
    return;
  }
  const auto provider = image::make_lensfun_optics_provider(database);
  expect(provider->info().available, "test Lensfun database loads");
  if (!provider->info().available) {
    return;
  }
  image::AssetMetadata metadata;
  metadata.make = "Nikon Corporation";
  metadata.model = "Nikon D850";
  metadata.normalized_make = "Nikon";
  metadata.normalized_model = "D850";
  metadata.lens_make = "Nikon";
  metadata.lens_model = "Nikon AF Nikkor 50mm f/1.4D";
  metadata.focal_length_mm = 50.0;
  metadata.focal_length_35mm = 50.0;
  metadata.aperture_f_number = 1.4;
  metadata.focus_distance_meters = 10.0;

  const auto profile_candidates = provider->profile_candidates(metadata);
  expect(!profile_candidates.empty(),
         "Lensfun enumerates profiles compatible with a matched camera");
  if (profile_candidates.empty()) {
    return;
  }

  auto pentax_metadata = metadata;
  pentax_metadata.make = "Pentax";
  pentax_metadata.model = "K10D";
  pentax_metadata.normalized_make = "Pentax";
  pentax_metadata.normalized_model = "K10D";
  expect(!provider->profile_candidates(pentax_metadata).empty(),
         "Lensfun profile enumeration accepts the Pentax K10D EXIF identity");

  const auto input = processed_linear_gradient();
  const auto result = provider->correct_reference_rgb(
      input, metadata, image::default_optics_settings());
  expect(result.receipt.status == image::OpticsProfileStatus::matched,
         "Lensfun matches the camera/lens profile from RAW metadata");
  expect(result.receipt.applied_distortion,
         "Lensfun applies calibrated distortion correction");
  expect(result.receipt.applied_tca,
         "Lensfun applies calibrated TCA correction");
  expect(result.receipt.applied_vignetting,
         "Lensfun applies calibrated vignetting correction");
  expect(result.receipt.applied_scaling,
         "Lensfun auto-scale is applied with geometry correction");
  expect(result.corrected_reference_rgb.has_value(),
         "an active Lensfun profile materializes a corrected raster");
  if (result.corrected_reference_rgb.has_value()) {
    expect(result.corrected_reference_rgb->dimensions == input.dimensions,
           "optics preserves the image canvas dimensions");
    expect(result.corrected_reference_rgb->samples != input.samples,
           "profile correction changes the synthetic gradient");
  }

  // The same calibrated profile must accept the host's scene-linear RAW
  // reference directly. Keep every fixture sample above display white so a
  // regression through the legacy u16 path is observable even when Lensfun's
  // geometric remap changes which source samples land at the canvas edge.
  image::SceneLinearRgbFrame scene_linear_input;
  scene_linear_input.dimensions = input.dimensions;
  scene_linear_input.row_stride_bytes =
      static_cast<std::size_t>(input.dimensions.width) * 3U * sizeof(float);
  scene_linear_input.samples.assign(
      static_cast<std::size_t>(input.dimensions.width) *
          input.dimensions.height * 3U,
      1.5F);
  const auto scene_linear_result = provider->correct_scene_linear_reference(
      scene_linear_input, metadata, image::default_optics_settings());
  expect(
      scene_linear_result.receipt.status ==
              image::OpticsProfileStatus::matched &&
          scene_linear_result.receipt.applied_distortion &&
          scene_linear_result.receipt.applied_tca &&
          scene_linear_result.receipt.applied_vignetting,
      "Lensfun applies its calibrated profile to a scene-linear RAW reference");
  expect(scene_linear_result.corrected_scene_linear_rgb.has_value(),
         "a calibrated float optics pass materializes a scene-linear frame");
  if (scene_linear_result.corrected_scene_linear_rgb.has_value()) {
    const auto &float_output = *scene_linear_result.corrected_scene_linear_rgb;
    expect(
        float_output.valid() &&
            float_output.dimensions == scene_linear_input.dimensions,
        "calibrated float optics preserves the scene-linear RGB frame layout");
    expect(
        std::any_of(float_output.samples.begin(), float_output.samples.end(),
                    [](const float sample) { return sample > 1.0F; }),
        "calibrated float optics preserves super-white RAW highlight headroom");
  }

  auto missing_distance_metadata = metadata;
  missing_distance_metadata.focus_distance_meters = 0.0;
  const auto far_distance_result = provider->correct_reference_rgb(
      input, missing_distance_metadata, image::default_optics_settings());
  expect(far_distance_result.receipt.applied_vignetting,
         "Lensfun retains ordinary vignetting correction when focus distance "
         "is absent");
  expect(far_distance_result.receipt.vignetting_used_distance_fallback,
         "Lensfun receipt discloses the far-distance vignetting approximation");

  auto manual_metadata = metadata;
  manual_metadata.lens_make.clear();
  manual_metadata.lens_model.clear();
  auto manual_settings = image::default_optics_settings();
  manual_settings.camera_profile_maker =
      profile_candidates.front().camera_maker;
  manual_settings.camera_profile_model =
      profile_candidates.front().camera_model;
  manual_settings.lens_profile_maker = profile_candidates.front().lens_maker;
  manual_settings.lens_profile_model = profile_candidates.front().lens_model;
  const auto manual_result =
      provider->correct_reference_rgb(input, manual_metadata, manual_settings);
  expect(manual_result.receipt.status == image::OpticsProfileStatus::matched,
         "an explicit camera/lens profile works without EXIF lens identity");
}

void optics_runs_before_preview_and_full_detail_preparation() {
  const FakeRgbSession session;
  const ReplacingOpticsProvider optics;
  const auto warm = image::prepare_warm_edit_preview(session, 8U, &optics);
  expect(warm.optics_receipt().status == image::OpticsProfileStatus::matched,
         "warm preview retains the applied optics receipt");
  expect(warm.optics_receipt().applied_distortion,
         "warm preview receives the provider's optical source");
  expect(optics.correction_count() == 1U,
         "warm preview applies optics once during preparation");
  expect(optics.last_settings().enabled,
         "warm preview sends enabled optics settings to its provider");

  const auto detail = image::prepare_full_edit_detail(session, &optics);
  expect(detail.optics_receipt().status == image::OpticsProfileStatus::matched,
         "full detail retains the applied optics receipt");
  expect(detail.optics_receipt().camera_profile == "Test camera",
         "full detail carries profile identity instead of a hidden transform");
  expect(optics.correction_count() == 2U,
         "full detail prepares an independent immutable source");

  const std::array no_nodes{image::AdjustmentNode{
      .node_id = "neutral-exposure",
      .parameters = image::ExposureAdjustment{},
  }};
  const auto proxy = warm.render_jpeg(no_nodes);
  expect(!proxy.bytes.empty(),
         "optically prepared warm preview still encodes normally");
}

} // namespace

int main() {
  optics_settings_are_explicit_and_provider_safe();
  scene_linear_optics_preserves_headroom_for_manual_correction();
  lensfun_adapter_applies_a_real_profile_when_a_test_database_is_available();
  optics_runs_before_preview_and_full_detail_preparation();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
