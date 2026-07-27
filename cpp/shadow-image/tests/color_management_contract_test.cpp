#include "contract_test_assertions.hpp"

#include <shadow/image/color_management.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

void icc_color_management_is_content_addressed_and_transfer_aware() {
  const auto linear_srgb = image::make_linear_srgb_icc_profile();
  const auto another_linear_srgb = image::make_linear_srgb_icc_profile();
  const auto display_srgb = image::make_display_srgb_icc_profile();
  const auto display_rec709 = image::make_display_rec709_icc_profile();
  expect(linear_srgb.info().id == another_linear_srgb.info().id,
         "equivalent generated ICC profiles have stable content identities");
  expect(linear_srgb.info().id != display_srgb.info().id,
         "linear and display sRGB profiles cannot share a cache identity");
  expect(display_rec709.info().id != display_srgb.info().id,
         "Rec.709 and sRGB transfers cannot share a cache identity");
  expect(display_srgb.serialized().size() ==
             display_srgb.info().serialized_bytes,
         "ICC profile exposes its complete canonical payload for export "
         "embedding");
  const auto round_tripped_display_srgb =
      image::load_icc_profile(display_srgb.serialized());
  expect(round_tripped_display_srgb.info().id == display_srgb.info().id,
         "serializing and reopening an ICC profile preserves its content "
         "identity");

  const auto identity = image::make_icc_transform(linear_srgb, linear_srgb);
  std::array<float, 6U> samples{0.18F, 0.5F, 1.2F, 0.0F, 0.25F, 0.75F};
  const auto before = samples;
  identity.apply_interleaved_rgb(samples);
  for (std::size_t index = 0U; index < samples.size(); ++index) {
    expect(std::abs(samples[index] - before[index]) < 1.0e-5F,
           "linear sRGB ICC identity transform preserves scene-linear samples");
  }

  const auto display_transform = image::make_icc_transform(
      linear_srgb, display_srgb,
      image::IccRenderingIntent::relative_colorimetric);
  const auto equivalent_display_transform = image::make_icc_transform(
      another_linear_srgb, round_tripped_display_srgb,
      image::IccRenderingIntent::relative_colorimetric);
  expect(display_transform.info().id == equivalent_display_transform.info().id,
         "equivalent ICC transforms have a stable cache identity");
  expect(display_transform.info().source.id == linear_srgb.info().id &&
             display_transform.info().destination.id == display_srgb.info().id,
         "ICC transform identity records both profile identities");
  expect(display_transform.black_point_compensation(),
         "ICC transform reports its black-point compensation policy");
  const auto no_bpc_transform = image::make_icc_transform(
      linear_srgb, display_srgb,
      image::IccRenderingIntent::relative_colorimetric, false);
  const auto perceptual_transform = image::make_icc_transform(
      linear_srgb, display_srgb, image::IccRenderingIntent::perceptual);
  expect(no_bpc_transform.info().id != display_transform.info().id &&
             !no_bpc_transform.black_point_compensation(),
         "black-point compensation participates in the ICC transform cache "
         "identity");
  expect(perceptual_transform.info().id != display_transform.info().id,
         "rendering intent participates in the ICC transform cache identity");

  image::IccTransformCache transform_cache(2U);
  const auto cached_display_transform =
      transform_cache.resolve(linear_srgb, display_srgb,
                              image::IccRenderingIntent::relative_colorimetric);
  const auto cached_equivalent_transform =
      transform_cache.resolve(another_linear_srgb, round_tripped_display_srgb,
                              image::IccRenderingIntent::relative_colorimetric);
  expect(cached_display_transform.info().id == display_transform.info().id &&
             cached_equivalent_transform.info().id ==
                 display_transform.info().id,
         "ICC transform cache preserves the complete transform contract");
  expect(transform_cache.capacity() == 2U && transform_cache.size() == 1U,
         "equivalent source and destination profile content reuse one cache "
         "entry");
  static_cast<void>(transform_cache.resolve(
      linear_srgb, display_srgb,
      image::IccRenderingIntent::relative_colorimetric, false));
  static_cast<void>(transform_cache.resolve(
      display_srgb, linear_srgb,
      image::IccRenderingIntent::relative_colorimetric));
  expect(transform_cache.size() == 2U,
         "ICC transform cache bounds distinct intent and direction entries");
  transform_cache.clear();
  expect(transform_cache.size() == 0U,
         "ICC transform cache can release its retained transforms");

  image::IccTransformCache disabled_transform_cache(0U);
  const auto uncached_transform =
      disabled_transform_cache.resolve(linear_srgb, display_srgb);
  expect(uncached_transform.info().id == display_transform.info().id &&
             disabled_transform_cache.size() == 0U,
         "zero-capacity ICC cache keeps the exact contract without retaining "
         "state");
  std::array<float, 3U> middle_gray{0.18F, 0.18F, 0.18F};
  display_transform.apply_interleaved_rgb(middle_gray);
  for (const auto encoded : middle_gray) {
    expect(std::abs(encoded - 0.461F) < 0.01F,
           "linear-to-display ICC transform applies the sRGB transfer curve");
  }

  const auto rec709_transform = image::make_icc_transform(
      linear_srgb, display_rec709,
      image::IccRenderingIntent::relative_colorimetric);
  std::array<float, 3U> rec709_middle_gray{0.18F, 0.18F, 0.18F};
  rec709_transform.apply_interleaved_rgb(rec709_middle_gray);
  for (const auto encoded : rec709_middle_gray) {
    expect(
        std::abs(encoded - 0.409F) < 0.01F,
        "linear-to-display ICC transform applies the Rec.709 transfer curve");
  }

  try {
    std::array<float, 2U> malformed{0.0F, 0.0F};
    identity.apply_interleaved_rgb(malformed);
    expect(false, "ICC transform rejects non-RGB sample counts");
  } catch (const std::invalid_argument &) {
    expect(true, "ICC transform reports malformed RGB sample counts");
  }
}


} // namespace

int main() {
  icc_color_management_is_content_addressed_and_transfer_aware();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
