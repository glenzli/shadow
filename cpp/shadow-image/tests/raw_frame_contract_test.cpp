#include "contract_test_assertions.hpp"

#include <shadow/image/raw_frame.hpp>

#include <cstdlib>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

void raw_frame_is_owned_unprocessed_and_bayer_guarded() {
  image::RawFrame frame;
  frame.descriptor.schema_version = image::raw_frame_schema_version;
  frame.descriptor.storage_dimensions = {4U, 2U};
  frame.descriptor.active_dimensions = {4U, 2U};
  frame.descriptor.sample_encoding =
      image::RawFrameSampleEncoding::uint16_native;
  frame.descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
  frame.descriptor.bayer_2x2 = {
      image::RawCfaColor::red,
      image::RawCfaColor::green,
      image::RawCfaColor::green,
      image::RawCfaColor::blue,
  };
  frame.descriptor.cfa_pattern = "RGGB";
  frame.descriptor.bits_per_sample = 14U;
  frame.descriptor.black_levels = {512U, 510U, 511U, 508U};
  frame.descriptor.white_levels = {16'383U, 16'383U, 16'383U, 16'383U};
  frame.descriptor.as_shot_neutral = {0.5, 1.0, 1.0, 0.75};
  frame.samples = {512U, 600U, 700U, 800U, 900U, 1'000U, 1'100U, 1'200U};
  expect(frame.valid(),
         "a complete owned RAW frame validates before any sensor processing");
  expect(frame.is_bayer_2x2(),
         "Bayer stages require an explicit two-by-two CFA layout");

  auto half_identified = frame;
  half_identified.descriptor.provider_id = "fixture";
  expect(!half_identified.valid(),
         "a RAW frame never carries half of a cache-visible provider identity");

  auto transformed = frame;
  transformed.descriptor.camera_to_linear_srgb_d65 = {
      1.2, -0.1, -0.1, -0.2, 1.3, -0.1, 0.0, -0.2, 1.2,
  };
  transformed.descriptor.has_camera_to_linear_srgb_d65 = true;
  expect(transformed.valid(), "a finite non-zero camera-to-linear-sRGB D65 "
                              "transform is part of a valid RAW frame");
  transformed.descriptor.camera_to_linear_srgb_d65 = {};
  expect(
      !transformed.valid(),
      "a declared camera-to-linear-sRGB transform cannot be an empty matrix");

  auto unknown_layout = frame;
  unknown_layout.descriptor.cfa_layout = image::RawFrameCfaLayout::unknown;
  expect(unknown_layout.valid() && !unknown_layout.is_bayer_2x2(),
         "an unknown CFA remains inspectable but cannot enter a Bayer-only "
         "algorithm");

  auto truncated = frame;
  truncated.samples.pop_back();
  expect(!truncated.valid(),
         "RAW frame validation rejects a non-owned/truncated sample plane");
}

void raw_frame_sensor_noise_calibration_is_explicit_and_fail_closed() {
  image::RawSensorNoiseCalibration unavailable;
  expect(unavailable.valid(),
         "an unavailable sensor-noise model is an explicit, valid absence "
         "rather than guessed data");

  image::RawSensorNoiseCalibration calibrated{
      .schema_version = image::raw_sensor_noise_calibration_schema_version,
      .model = image::RawSensorNoiseModel::poisson_gaussian_per_cfa,
      .source =
          image::RawSensorNoiseCalibrationSource::provider_calibration_profile,
      .iso_sensitivity = 800.0,
      .read_noise_stddev_dn = {2.1, 1.9, 1.9, 2.2},
      .shot_noise_variance_per_dn = {0.72, 0.71, 0.71, 0.74},
  };
  expect(calibrated.valid(), "a per-CFA Poisson-Gaussian model records a "
                             "provider-resolved calibration in DN units");

  auto invalid = calibrated;
  invalid.shot_noise_variance_per_dn[3U] = 0.0;
  expect(!invalid.valid(), "sensor-noise calibration rejects a non-positive "
                           "shot-noise variance coefficient");
}

} // namespace

int main() {
  raw_frame_is_owned_unprocessed_and_bayer_guarded();
  raw_frame_sensor_noise_calibration_is_explicit_and_fail_closed();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
