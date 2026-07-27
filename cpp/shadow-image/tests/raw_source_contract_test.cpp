#include "decoder_contract_test_support.hpp"

#include <shadow/image/color_management.hpp>
#include <shadow/image/decoder.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/edit.hpp>
#include <shadow/image/raw_development.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <stop_token>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

void pending_corrections_are_explicit() {
  image::PendingCorrections empty;
  expect(!empty.has_pending(), "empty correction state must not be pending");

  image::PendingCorrections stage_three{{0U, 0U, 76U}};
  expect(stage_three.has_pending(),
         "a DNG opcode list must be reported as pending");
}

void raw_development_receipt_is_explicitly_absent_until_a_provider_records_it() {
  const image::PixelBuffer generic;
  expect(!generic.raw_development_receipt.recorded(),
         "generic processed RGB never pretends to carry RAW provenance");
  expect(image::raw_development_receipt_schema_version == 1U,
         "RAW development receipt schema is explicitly versioned");
}

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

void raw_development_plan_is_canonical_and_capability_negotiated() {
  const auto detail = image::default_raw_development_plan();
  const auto preview = image::preview_raw_development_plan();
  expect(
      detail.intent == image::RawDevelopmentIntent::detail &&
          detail.quality == image::RawDevelopmentQuality::balanced &&
          detail.dng_opcode_policy == image::DngOpcodePolicy::provider_default,
      "default RAW development plan is a neutral full-detail provider request");
  expect(preview.intent == image::RawDevelopmentIntent::preview &&
             preview.quality == detail.quality &&
             preview.dng_opcode_policy == detail.dng_opcode_policy,
         "preview RAW development plan changes intent without changing source "
         "policy");
  expect(image::raw_development_plan_identity(detail) ==
             "shadow-raw-plan-v1;intent=detail;quality=balanced;opcodes="
             "provider-default;nr=provider-default;highlights=provider-default",
         "RAW development plan identity is canonical and cache-visible");

  image::RawDevelopmentCapabilities capabilities;
  capabilities.schema_version =
      image::raw_development_capabilities_schema_version;
  capabilities.available = true;
  capabilities.supported_intents =
      image::raw_development_intent_mask(image::RawDevelopmentIntent::preview) |
      image::raw_development_intent_mask(image::RawDevelopmentIntent::detail);
  capabilities.supported_qualities = image::raw_development_quality_mask(
      image::RawDevelopmentQuality::balanced);
  capabilities.supported_dng_opcode_policies =
      image::dng_opcode_policy_mask(image::DngOpcodePolicy::provider_default);
  capabilities.supported_noise_reduction_intents =
      image::raw_noise_reduction_intent_mask(
          image::RawNoiseReductionIntent::provider_default);
  capabilities.supported_highlight_recovery_intents =
      image::raw_highlight_recovery_intent_mask(
          image::RawHighlightRecoveryIntent::provider_default);
  const auto accepted =
      image::negotiate_raw_development_plan(detail, capabilities);
  expect(accepted.accepted() && accepted.exact() &&
             accepted.requested == detail && accepted.effective == detail,
         "capability negotiation accepts an exactly supported RAW plan");

  auto unsupported_quality = detail;
  unsupported_quality.quality = image::RawDevelopmentQuality::high;
  const auto quality_rejected =
      image::negotiate_raw_development_plan(unsupported_quality, capabilities);
  expect(
      !quality_rejected.accepted() &&
          image::raw_development_plan_aspect_contains(
              quality_rejected.unresolved,
              image::RawDevelopmentPlanAspect::quality),
      "a provider cannot silently substitute an unsupported RAW quality tier");

  auto unsupported_opcode_policy = detail;
  unsupported_opcode_policy.dng_opcode_policy =
      image::DngOpcodePolicy::require_applied;
  const auto opcode_rejected = image::negotiate_raw_development_plan(
      unsupported_opcode_policy, capabilities);
  expect(!opcode_rejected.accepted() &&
             image::raw_development_plan_aspect_contains(
                 opcode_rejected.unresolved,
                 image::RawDevelopmentPlanAspect::dng_opcode_policy),
         "a provider cannot silently claim required DNG opcode application");

  auto future_schema = detail;
  future_schema.schema_version += 1U;
  const auto schema_rejected =
      image::negotiate_raw_development_plan(future_schema, capabilities);
  expect(!schema_rejected.accepted() &&
             image::raw_development_plan_aspect_contains(
                 schema_rejected.unresolved,
                 image::RawDevelopmentPlanAspect::schema),
         "unknown RAW development plan schemas fail closed");
  try {
    static_cast<void>(image::raw_development_plan_identity(future_schema));
    expect(
        false,
        "unknown RAW development plan schemas cannot produce a cache identity");
  } catch (const std::invalid_argument &) {
    expect(true, "invalid RAW plan identity reports an invalid argument");
  }
}

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

void largest_decodable_preview_wins() {
  const std::array previews{
      image::PreviewDescriptor{
          .id = 2,
          .format = image::PreviewFormat::bitmap,
          .dimensions = {160, 120},
          .bits_per_channel = 8,
          .channels = 3,
          .encoded_bytes = 57'600,
          .decodable = true,
      },
      image::PreviewDescriptor{
          .id = 7,
          .format = image::PreviewFormat::jpeg,
          .dimensions = {3'872, 2'592},
          .bits_per_channel = 8,
          .channels = 3,
          .encoded_bytes = 1'285'213,
          .decodable = true,
      },
      image::PreviewDescriptor{
          .id = 9,
          .format = image::PreviewFormat::unknown,
          .dimensions = {8'000, 6'000},
          .bits_per_channel = 8,
          .channels = 3,
          .encoded_bytes = 2'000'000,
          .decodable = false,
      },
  };

  const auto selected = image::select_best_preview(previews);
  expect(selected.has_value(), "a decodable preview should be selected");
  expect(selected == 7U,
         "selection returns the provider preview id, not the vector index");
}

void no_decodable_preview_is_a_valid_state() {
  const std::array previews{
      image::PreviewDescriptor{
          .id = 0,
          .format = image::PreviewFormat::unknown,
          .dimensions = {},
          .bits_per_channel = 0,
          .channels = 0,
          .encoded_bytes = 0,
          .decodable = false,
      },
  };
  expect(!image::select_best_preview(previews).has_value(),
         "files without an embedded preview must remain importable");
}

void jpeg_raster_provider_uses_the_common_non_destructive_graph() {
  // Keep the public JPEG contract hermetic: a developer's installed local
  // decoder module may intentionally be stale and is covered by the dedicated
  // fail-closed plugin tests above.
  const auto provider =
      image::make_photo_decoder_provider(std::filesystem::path{});
  const auto session = provider->open(SHADOW_TEST_JPEG_PATH);
  expect(provider->info().id == "shadow-photo-router",
         "normal application photo entry point is provider-neutral");
  expect(session->capabilities().metadata &&
             session->capabilities().reference_rgb &&
             !session->capabilities().raw_frame,
         "JPEG exposes metadata and editable RGB but never pretends to have a "
         "sensor RAW frame");
  expect(session->previews().empty(),
         "JPEG source relies on the colour-managed generated proxy rather than "
         "an unrotated source byte preview");
  const image::PixelBuffer decoded =
      session->render_reference_rgb_for_preview(1'024U);
  expect(decoded.reference == image::RgbBufferReference::decoded_raster &&
             decoded.transfer_function == image::RgbTransferFunction::linear &&
             decoded.primaries == image::RgbPrimaries::srgb_rec709_d65 &&
             decoded.bits_per_channel == 16U && decoded.channels == 3U,
         "JPEG is colour-managed into the common linear RGB contract without "
         "being labeled RAW");
  expect(!decoded.raw_development_receipt.recorded(),
         "JPEG never fabricates a RAW development receipt");

  const image::ProxyRequest request{.max_edge = 1'024U, .jpeg_quality = 90U};
  const auto reference = image::render_reference_proxy_jpeg(*session, request);
  const std::array neutral_nodes{
      image::AdjustmentNode{
          .node_id = "neutral-raster-exposure",
          .parameters = image::ExposureAdjustment{},
      },
  };
  const auto edited = image::render_edited_reference_proxy_jpeg(
      *session, neutral_nodes, request);
  expect(edited.bytes == reference.bytes,
         "JPEG follows the exact same neutral edit graph and display boundary "
         "as its reference proxy");

  const auto warm = image::prepare_warm_edit_preview(*session, 1'024U);
  std::stop_source cancellation;
  expect(cancellation.request_stop(),
         "first preview cancellation request succeeds");
  const auto cancelled = warm.render_jpeg_cancellable(neutral_nodes, 90U,
                                                      cancellation.get_token());
  const auto cancelled_analysis = warm.render_jpeg_with_analysis_cancellable(
      neutral_nodes, 90U, cancellation.get_token());
  expect(cancelled.cancelled() && cancelled_analysis.cancelled(),
         "pre-cancelled CPU/Metal warm previews return explicit Cancelled "
         "without partial output");
  expect(!warm.render_jpeg(neutral_nodes, 90U).bytes.empty(),
         "a cancelled request does not poison the immutable warm session");
}

} // namespace

int main() {
  pending_corrections_are_explicit();
  raw_development_receipt_is_explicitly_absent_until_a_provider_records_it();
  raw_frame_is_owned_unprocessed_and_bayer_guarded();
  sensor_clipping_marks_sensor_endpoints_without_confusing_dark_content();
  raw_frame_sensor_noise_calibration_is_explicit_and_fail_closed();
  bayer_bilinear_demosaic_keeps_the_sensor_domain_explicit();
  raw_development_plan_is_canonical_and_capability_negotiated();
  icc_color_management_is_content_addressed_and_transfer_aware();
  largest_decodable_preview_wins();
  no_decodable_preview_is_a_valid_state();
  jpeg_raster_provider_uses_the_common_non_destructive_graph();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
