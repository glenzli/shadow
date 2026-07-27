#include "decoder_contract_test_support.hpp"

#include <shadow/image/decoder.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/private_decoder_plugin.hpp>
#include <shadow/image/raw_development.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

void private_decoder_plugin_abi_is_explicit_and_fail_closed() {
  try {
    image::validate_private_decoder_plugin_interface_contract(
        image::private_decoder_plugin_interface_contract_token);
    expect(true, "current private decoder interface contract is accepted");
  } catch (const image::DecodeError &) {
    expect(false, "current private decoder interface contract must not throw");
  }
  try {
    image::validate_private_decoder_plugin_interface_contract(
        image::private_decoder_plugin_interface_contract_token ^ 1U);
    expect(false, "stale private decoder interface contract must be rejected");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::unsupported,
           "private decoder interface contract mismatch reports unsupported");
  }

  const image::PrivateDecoderPluginDescriptor valid{
      .abi_version = image::private_decoder_plugin_abi_version,
      .raw_development_plan_schema_version =
          image::raw_development_plan_schema_version,
      .raw_frame_schema_version = image::raw_frame_schema_version,
      .plugin_id = "nikon-local",
      .plugin_version = "0.1.0",
  };
  try {
    image::validate_private_decoder_plugin_descriptor(valid);
    expect(true, "private decoder plugin ABI accepts a valid descriptor");
  } catch (const image::DecodeError &) {
    expect(false, "valid private decoder plugin descriptor must not throw");
  }

  auto future_abi = valid;
  future_abi.abi_version += 1U;
  try {
    image::validate_private_decoder_plugin_descriptor(future_abi);
    expect(false, "future private decoder ABI must be rejected");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::unsupported,
           "private decoder ABI mismatch reports unsupported");
  }

  auto legacy_abi = valid;
  legacy_abi.abi_version -= 1U;
  try {
    image::validate_private_decoder_plugin_descriptor(legacy_abi);
    expect(false, "legacy private decoder ABI must be rejected before provider "
                  "construction");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::unsupported,
           "legacy private decoder ABI fails closed as unsupported");
  }

  auto legacy_plan_schema = valid;
  legacy_plan_schema.raw_development_plan_schema_version -= 1U;
  try {
    image::validate_private_decoder_plugin_descriptor(legacy_plan_schema);
    expect(false, "private decoder plan schema must be rejected before "
                  "provider construction");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::unsupported,
           "private decoder plan schema mismatch reports unsupported");
  }

  auto legacy_raw_frame_schema = valid;
  legacy_raw_frame_schema.raw_frame_schema_version -= 1U;
  try {
    image::validate_private_decoder_plugin_descriptor(legacy_raw_frame_schema);
    expect(false, "private decoder RAW frame schema must be rejected before "
                  "construction");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::unsupported,
           "private decoder RAW frame schema mismatch fails closed as "
           "unsupported");
  }

  auto invalid_id = valid;
  invalid_id.plugin_id = "vendor sdk";
  try {
    image::validate_private_decoder_plugin_descriptor(invalid_id);
    expect(false, "private decoder identifiers cannot contain spaces");
  } catch (const image::DecodeError &) {
    expect(true, "invalid private decoder id is rejected");
  }
}

void stale_private_decoder_plugin_is_rejected_before_construction() {
#if defined(SHADOW_TEST_STALE_PRIVATE_DECODER_PLUGIN_PATH)
  try {
    static_cast<void>(image::load_private_decoder_plugin(
        SHADOW_TEST_STALE_PRIVATE_DECODER_PLUGIN_PATH));
    expect(false,
           "stale private decoder plugin must be rejected before construction");
  } catch (const image::DecodeError &error) {
    expect(
        error.code() == image::DecodeErrorCode::unsupported &&
            std::string_view(error.what())
                    .find(
                        image::
                            private_decoder_plugin_interface_contract_symbol) !=
                std::string_view::npos,
        "a plugin missing the exact interface seal fails closed before its "
        "aborting factory");
  }
#else
  expect(false,
         "stale private decoder plugin test target path must be configured");
#endif
}

void private_decoder_plugin_loads_an_explicit_local_module() {
#if defined(SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH)
  const auto provider = image::load_private_decoder_plugin(
      SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH);
  expect(provider->info().id == "private.test-private-provider.fixture",
         "private plugin identity remains namespaced by its explicit local "
         "module");
  expect(provider->info().version.starts_with("1.0.0;abi=1;contract=") &&
             provider->info().version.find(";plan=1;frame=1;wrapped=") !=
                 std::string::npos &&
             provider->info().version.find(";module=") != std::string::npos &&
             provider->info().version.size() <= 128U,
         "private plugin version, ABI seal, wrapped provider and local module "
         "identity stay bounded");
  const auto session = provider->open("does-not-need-to-exist.raw");
  expect(session->metadata().model == "Private decoder test fixture",
         "private provider session stays callable through the host adapter");
  expect(session->raw_development_capabilities().available &&
             session->raw_development_capabilities().raw_frame &&
             session->raw_development_capabilities().supports(
                 image::default_raw_development_plan()),
         "private provider exposes plan capability negotiation through the "
         "host adapter");
  const auto raw_frame = session->decode_raw_frame();
  expect(raw_frame.valid() && raw_frame.is_bayer_2x2() &&
             raw_frame.samples ==
                 std::vector<std::uint16_t>({1'024U, 1'100U, 1'100U, 900U}) &&
             raw_frame.descriptor.has_camera_to_xyz_d50 &&
             raw_frame.descriptor.camera_to_xyz_d50 ==
                 std::array<double, 9U>{
                     0.70,
                     0.20,
                     0.10,
                     0.10,
                     0.80,
                     0.10,
                     0.05,
                     0.15,
                     0.80,
                 } &&
             raw_frame.descriptor.provider_id == provider->info().id &&
             raw_frame.descriptor.provider_version.starts_with(
                 provider->info().version + ";frame="),
         "private plugin RawFrame keeps CFA calibration and host-bound cache "
         "provenance");
  const auto pixels = session->render_reference_rgb();
  expect(pixels.samples == std::vector<std::uint16_t>({0U, 1U, 2U, 3U, 4U, 5U}),
         "private plugin reference RGB crosses the provider-neutral contract");
  expect(pixels.raw_development_receipt.uses_current_schema() &&
             pixels.raw_development_receipt.provider_id ==
                 "private.test-private-provider.fixture" &&
             pixels.raw_development_receipt.provider_version ==
                 provider->info().version &&
             pixels.raw_development_receipt.library_version ==
                 "private-fixture-sdk",
         "private plugin receipts are bound to the host provider identity "
         "without hiding SDK detail");
  const auto explicit_detail_plan = image::default_raw_development_plan();
  const auto explicit_detail =
      session->render_reference_rgb(explicit_detail_plan);
  expect(explicit_detail.raw_development_receipt.requested_plan ==
                 explicit_detail_plan &&
             explicit_detail.raw_development_receipt.effective_plan ==
                 explicit_detail_plan &&
             explicit_detail.raw_development_receipt.requested_plan_identity ==
                 image::raw_development_plan_identity(explicit_detail_plan),
         "private provider plan render binds an auditable requested and "
         "effective plan receipt");
  const auto preview_pixels = session->render_reference_rgb_for_preview(1U);
  expect(preview_pixels.dimensions == image::Dimensions{1U, 1U} &&
             preview_pixels.samples ==
                 std::vector<std::uint16_t>({9U, 8U, 7U}) &&
             preview_pixels.raw_development_receipt.provider_id ==
                 "private.test-private-provider.fixture",
         "private plugin fast preview is forwarded instead of falling back to "
         "full RGB");
  const auto explicit_preview_plan = image::preview_raw_development_plan();
  const auto explicit_preview =
      session->render_reference_rgb_for_preview(1U, explicit_preview_plan);
  expect(explicit_preview.raw_development_receipt.requested_plan ==
                 explicit_preview_plan &&
             explicit_preview.raw_development_receipt.half_size,
         "private provider plan-aware preview retains preview intent in its "
         "receipt");
  auto unsupported_quality = explicit_detail_plan;
  unsupported_quality.quality = image::RawDevelopmentQuality::high;
  try {
    static_cast<void>(session->render_reference_rgb(unsupported_quality));
    expect(false,
           "private provider must reject a RAW plan it did not negotiate");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::unsupported,
           "private provider rejects unsupported RAW plan quality before "
           "rendering");
  }
#else
  expect(false, "private decoder plugin test target path must be configured");
#endif
}

void private_decoder_router_prefers_an_explicit_local_module() {
#if defined(SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH) &&                        \
    defined(SHADOW_TEST_LIBRAW_DUMMY_PRIVATE_DECODER_PLUGIN_PATH)
  const auto router = image::make_photo_decoder_provider(
      SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH);
  expect(router->info().version.find(";private=") != std::string::npos,
         "photo router includes an explicit private provider in its cache "
         "identity");
  expect(router->info().version.find(";private_module=") != std::string::npos,
         "photo router includes the local private module fingerprint in its "
         "cache identity");
  const auto fixture_session = router->open("fixture-private-provider.raw");
  expect(fixture_session->metadata().model == "Private decoder test fixture",
         "photo router tries the explicit private provider before LibRaw for "
         "RAW sources");

  const auto dummy = image::load_private_decoder_plugin(
      SHADOW_TEST_LIBRAW_DUMMY_PRIVATE_DECODER_PLUGIN_PATH);
  expect(
      dummy->info().id == "private.libraw-dummy.libraw",
      "LibRaw dummy provider loads through the same private module contract");
#else
  expect(false, "private decoder router fixture paths must be configured");
#endif
}

} // namespace

int main() {
  private_decoder_plugin_abi_is_explicit_and_fail_closed();
  stale_private_decoder_plugin_is_rejected_before_construction();
  private_decoder_plugin_loads_an_explicit_local_module();
  private_decoder_router_prefers_an_explicit_local_module();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
