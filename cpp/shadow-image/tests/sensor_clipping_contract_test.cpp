#include "contract_test_assertions.hpp"

#include <shadow/image/raw_frame.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <cstddef>

#include <cstdlib>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

void sensor_clipping_marks_sensor_endpoints_without_confusing_dark_content() {
  image::RawFrame frame;
  frame.descriptor.schema_version = image::raw_frame_schema_version;
  frame.descriptor.storage_dimensions = {4U, 4U};
  frame.descriptor.active_dimensions = {4U, 4U};
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
  frame.descriptor.bits_per_sample = 12U;
  frame.descriptor.black_levels = {100U, 100U, 100U, 100U};
  frame.descriptor.white_levels = {1'000U, 1'000U, 1'000U, 1'000U};
  frame.descriptor.as_shot_neutral = {0.5, 1.0, 1.0, 0.75};
  frame.samples.assign(16U, 100U);
  frame.samples[static_cast<std::size_t>(1U) * 4U + 1U] = 1'000U;

  const auto mask = image::project_sensor_clipping_mask(frame, {4U, 4U});
  expect(mask.valid(),
         "sensor clipping projection returns a self-consistent mask");
  expect(mask.highlight_pixel_count == 1U && mask.shadow_pixel_count == 15U &&
             (mask.samples[5U] & image::sensor_highlight_clipped) != 0U &&
             (mask.samples[5U] & image::sensor_shadow_clipped) == 0U,
         "white-level samples mark an irrecoverable highlight while "
         "black-floor areas remain distinct");

  frame.descriptor.orientation = 5;
  frame.samples.assign(16U, 500U);
  frame.samples[0U] = 1'000U;
  const auto rotated = image::project_sensor_clipping_mask(frame, {4U, 4U});
  expect(rotated.valid() &&
             (rotated.samples[12U] & image::sensor_highlight_clipped) != 0U,
         "LibRaw's 90-degree counterclockwise orientation maps RAW diagnostics "
         "into display space");

  // A downsampled diagnostic bin is an exact sensor-domain reduction: one
  // clipped sensor sample wins the highlight flag, while a shadow flag
  // survives only when every source sample in the bin is at the black floor.
  frame.descriptor.storage_dimensions = {6U, 4U};
  frame.descriptor.active_dimensions = {6U, 4U};
  frame.descriptor.orientation = 5;
  frame.samples.assign(24U, 100U);
  frame.samples[0U] = 1'000U;
  const auto reduced_rotated =
      image::project_sensor_clipping_mask(frame, {2U, 3U});
  expect(reduced_rotated.valid() &&
             reduced_rotated.highlight_pixel_count == 1U &&
             reduced_rotated.shadow_pixel_count == 5U &&
             (reduced_rotated.samples[4U] & image::sensor_highlight_clipped) !=
                 0U &&
             (reduced_rotated.samples[4U] & image::sensor_shadow_clipped) == 0U,
         "downsampled rotated clipping diagnostics retain exact any-highlight "
         "and all-shadow semantics");
}

} // namespace

int main() {
  sensor_clipping_marks_sensor_endpoints_without_confusing_dark_content();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
