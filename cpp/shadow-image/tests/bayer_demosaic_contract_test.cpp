#include "contract_test_assertions.hpp"

#include <shadow/image/decoder_error.hpp>
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
  expect(frame.valid(),
         "constant Bayer fixture is a valid unprocessed RAW frame");

  const auto output = image::demosaic_bayer_bilinear(frame);
  expect(output.valid(),
         "bilinear Bayer demosaic produces a valid camera-linear RGB frame");
  expect(output.receipt.algorithm ==
                 image::RawDemosaicAlgorithm::bayer_bilinear_v1 &&
             output.receipt.black_subtraction_applied &&
             output.receipt.white_level_normalization_applied &&
             !output.receipt.white_balance_applied &&
             !output.receipt.dng_opcodes_applied,
         "Bayer demosaic receipt never overclaims white balance or DNG opcode "
         "application");
  for (std::size_t pixel = 0U; pixel < 16U; ++pixel) {
    const auto index = pixel * 3U;
    expect(std::abs(output.samples[index] - 0.2F) < 1.0e-6F &&
               std::abs(output.samples[index + 1U] - 0.4F) < 1.0e-6F &&
               std::abs(output.samples[index + 2U] - 0.8F) < 1.0e-6F,
           "bilinear Bayer reconstruction preserves per-CFA black/white "
           "normalized camera RGB");
  }

  auto non_bayer = frame;
  non_bayer.descriptor.cfa_layout = image::RawFrameCfaLayout::unknown;
  try {
    static_cast<void>(image::demosaic_bayer_bilinear(non_bayer));
    expect(false, "Bayer demosaic must reject an unknown CFA layout");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::unsupported_layout,
           "unknown CFA layout is rejected before Bayer processing");
  }
}

} // namespace

int main() {
  bayer_bilinear_demosaic_keeps_the_sensor_domain_explicit();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
