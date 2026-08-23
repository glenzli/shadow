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

void highlight_chroma_risk_marks_disagreement_and_shared_terminal_shoulder() {
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
  frame.descriptor.white_levels = {1'000U, 1'000U, 1'000U, 1'000U};
  // The source developer uses a camera-supplied linear-response ceiling when it
  // exists, rather than treating the container's physical white word as the
  // start of the usable shoulder. This is the important Sony-style case: the
  // physical values are still below 1,000, but their calibrated response has
  // already run out of chroma headroom.
  frame.descriptor.has_linear_response_limits = true;
  frame.descriptor.linear_response_limits = {800U, 800U, 800U, 800U};
  frame.descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
  frame.samples.assign(16U, 500U);
  for (std::uint32_t y = 0U; y < 4U; ++y) {
    for (std::uint32_t x = 0U; x < 4U; ++x) {
      const auto index = static_cast<std::size_t>(y) * 4U + x;
      if ((x & 1U) == 0U && (y & 1U) == 0U) {
        frame.samples[index] = 795U; // red
      } else if ((x & 1U) != (y & 1U)) {
        frame.samples[index] = 790U; // green
      }
    }
  }

  const auto shoulder = image::project_highlight_chroma_risk_map(frame, {2U, 2U});
  expect(shoulder.valid() && shoulder.samples[0U] > 200U,
         "two near-white CFA channels with a lagging third channel retain a strong "
         "continuous chroma-risk signal before physical white");

  // The unreliable response shoulder begins before the terminal plateau.  This
  // two-channel case models the lower-intensity coloured halo surrounding a
  // clipped Sony sun core: it must receive a gradual risk signal rather than
  // waiting until the visible false colour is already severe.
  for (std::uint32_t y = 0U; y < 4U; ++y) {
    for (std::uint32_t x = 0U; x < 4U; ++x) {
      const auto index = static_cast<std::size_t>(y) * 4U + x;
      frame.samples[index] = ((x & 1U) == (y & 1U)) ? 720U : 620U;
    }
  }
  const auto early_disagreement = image::project_highlight_chroma_risk_map(frame, {2U, 2U});
  expect(early_disagreement.valid() && early_disagreement.samples[0U] > 60U,
         "two CFA channels entering the early calibrated shoulder receive a gradual risk signal");

  frame.samples.assign(16U, 650U);
  const auto ordinary_highlight = image::project_highlight_chroma_risk_map(frame, {2U, 2U});
  expect(ordinary_highlight.valid() && ordinary_highlight.samples[0U] == 0U,
         "well-exposed values below the calibrated shoulder retain their original chroma");

  for (std::uint32_t y = 0U; y < 4U; ++y) {
    for (std::uint32_t x = 0U; x < 4U; ++x) {
      frame.samples[static_cast<std::size_t>(y) * 4U + x] = 795U;
    }
  }
  const auto shared_terminal = image::project_highlight_chroma_risk_map(frame, {2U, 2U});
  expect(shared_terminal.valid() && shared_terminal.samples[0U] > 240U,
         "a shared terminal CFA shoulder is chroma-unreliable even without channel disagreement");

  for (std::uint32_t y = 0U; y < 4U; ++y) {
    for (std::uint32_t x = 0U; x < 4U; ++x) {
      const auto index = static_cast<std::size_t>(y) * 4U + x;
      frame.samples[index] = ((x & 1U) == 0U && (y & 1U) == 0U) ? 795U : 500U;
    }
  }
  const auto saturated_colour = image::project_highlight_chroma_risk_map(frame, {2U, 2U});
  expect(saturated_colour.valid() && saturated_colour.samples[0U] == 0U,
         "a single near-white CFA channel remains a potentially intentional saturated colour");
}

} // namespace

int main() {
  sensor_clipping_marks_sensor_endpoints_without_confusing_dark_content();
  highlight_chroma_risk_marks_disagreement_and_shared_terminal_shoulder();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
