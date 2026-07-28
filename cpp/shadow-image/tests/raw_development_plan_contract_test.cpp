#include "contract_test_assertions.hpp"

#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_development_receipt.hpp>
#include <shadow/image/reference_pixels.hpp>

#include <stdexcept>

#include <cstdlib>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

void raw_development_receipt_is_explicitly_absent_until_a_provider_records_it() {
  const image::PixelBuffer generic;
  expect(!generic.raw_development_receipt.recorded(),
         "generic processed RGB never pretends to carry RAW provenance");
  expect(image::raw_development_receipt_schema_version == 1U,
         "RAW development receipt schema is explicitly versioned");
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

} // namespace

int main() {
  raw_development_receipt_is_explicitly_absent_until_a_provider_records_it();
  raw_development_plan_is_canonical_and_capability_negotiated();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
