#include "contract_test_assertions.hpp"
#include "optics_test_fixture.hpp"
#include "processed_rgb_session_fixture.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edited_proxy_rendering.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_development_receipt.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::FakeRgbSession;
using shadow::image::test_support::processed_linear_gradient;
using shadow::image::test_support::ReplacingOpticsProvider;
using shadow::image::test_support::RetainedRgbSession;


void raw_development_receipt_survives_prepared_edit_sessions() {
  auto source = processed_linear_gradient(8U, 4U);
  const auto source_plan = image::default_raw_development_plan();
  source.raw_development_receipt = image::RawDevelopmentReceipt{
      .schema_version = image::raw_development_receipt_schema_version,
      .provider_id = "fixture-provider",
      .provider_version = "fixture-provider-v1",
      .library_version = "fixture-library-v1",
      .development_settings_signature = "fixture-request-v1",
      .requested_plan_identity =
          image::raw_development_plan_identity(source_plan),
      .effective_plan_identity =
          image::raw_development_plan_identity(source_plan),
      .requested_plan = source_plan,
      .effective_plan = source_plan,
      .plan_negotiation_status =
          image::RawDevelopmentPlanNegotiationStatus::accepted,
      .processed_linear_reference_contract_version = 7U,
      .declared_image_dimensions = {8U, 4U},
      .rendered_dimensions = {8U, 4U},
      .orientation = 5,
      .half_size = true,
      .use_camera_white_balance = true,
      .use_camera_matrix = true,
      .output_bits_per_channel = 16U,
      .output_color = 1,
      .gamma_inverse_power = 1.0,
      .gamma_linear_toe_slope = 1.0,
      .process_warnings = 0x1024U,
  };
  const RetainedRgbSession session(std::move(source));

  const auto warm = image::prepare_warm_edit_preview(session, 8U);
  expect(warm.raw_development_receipt().recorded() &&
             warm.raw_development_receipt().provider_id == "fixture-provider" &&
             warm.raw_development_receipt().half_size &&
             warm.raw_development_receipt().process_warnings == 0x1024U,
         "warm preparation retains RAW development provenance after pixel "
         "conversion");

  const auto detail = image::prepare_full_edit_detail(session);
  expect(detail.raw_development_receipt().recorded() &&
             detail.raw_development_receipt().provider_version ==
                 "fixture-provider-v1" &&
             detail.raw_development_receipt().orientation == 5 &&
             detail.raw_development_receipt().rendered_dimensions ==
                 image::Dimensions{8U, 4U} &&
             detail.raw_development_receipt().effective_plan == source_plan,
         "full-detail preparation retains RAW development provenance with its "
         "source raster");

  const ReplacingOpticsProvider discarding_optics;
  const auto detail_after_optics =
      image::prepare_full_edit_detail(session, &discarding_optics);
  expect(detail_after_optics.raw_development_receipt().uses_current_schema() &&
             detail_after_optics.raw_development_receipt().provider_id ==
                 "fixture-provider" &&
             detail_after_optics.raw_development_receipt().process_warnings ==
                 0x1024U,
         "full-detail preparation retains source provenance when an optics "
         "provider replaces pixels");
}


void warm_edit_preview_decodes_once_and_renders_repeatedly() {
  const FakeRgbSession session;
  const auto warm = image::prepare_warm_edit_preview(session, 4);
  expect(session.reference_render_count() == 1U,
         "warm preparation renders the RAW once");
  expect(warm.dimensions() == image::Dimensions{4, 2},
         "warm working proxy is max-edge bounded");
  expect(warm.max_edge() == 4U,
         "warm working proxy remembers its resource bound");

  const std::array neutral_nodes{
      image::AdjustmentNode{
          .node_id = "exposure",
          .parameters = image::ExposureAdjustment{},
      },
  };
  auto adjusted_nodes = neutral_nodes;
  adjusted_nodes[0].parameters = image::ExposureAdjustment{1.0};

  const auto neutral = warm.render_jpeg(neutral_nodes, 90);
  const auto adjusted = warm.render_jpeg(adjusted_nodes, 90);
  const auto neutral_rgb8 = warm.render_rgb8(neutral_nodes);
  const auto adjusted_rgb8 = warm.render_rgb8(adjusted_nodes);
  expect(session.reference_render_count() == 1U,
         "repeated warm renders never ask the decoder for pixels again");
  expect(neutral.dimensions == image::Dimensions{4, 2},
         "warm output dimensions stay fixed");
  expect(adjusted.dimensions == neutral.dimensions,
         "all warm renders share working dimensions");
  expect(adjusted.bytes != neutral.bytes,
         "warm renders apply each requested edit independently");
  expect(neutral_rgb8.format == image::PreviewFormat::bitmap &&
             neutral_rgb8.bits_per_channel == 8U && neutral_rgb8.channels == 3U &&
             neutral_rgb8.bytes.size() == 4U * 2U * 3U,
         "interactive warm output is tightly packed display-sRGB RGB8");
  expect(adjusted_rgb8.bytes != neutral_rgb8.bytes,
         "RGB8 warm renders apply each requested edit without JPEG encoding");

  const auto one_shot_adjusted = image::render_edited_reference_proxy_jpeg(
      session, adjusted_nodes,
      image::ProxyRequest{.max_edge = 4, .jpeg_quality = 90});
  expect(
      adjusted.bytes == one_shot_adjusted.bytes,
      "linear affine edits commute with the warm proxy's linear downsampling");
  expect(session.reference_render_count() == 2U,
         "only the one-shot comparison decodes again");
}

void rotated_raw_preview_preserves_native_effect_radius() {
  // LibRaw returns its processed raster in output orientation. This fixture
  // mirrors a camera whose metadata still advertises an 8x4 sensor frame while
  // the rendered RGB has been transposed to 4x8. A matching already-oriented
  // metadata fixture must produce exactly the same native-pixel denoise
  // footprint and therefore the same warm-preview bytes.
  const auto source = processed_linear_gradient(4U, 8U);
  image::AssetMetadata rotated_metadata;
  rotated_metadata.raw_dimensions = {8U, 4U};
  rotated_metadata.image_dimensions = {8U, 4U};
  rotated_metadata.orientation = 5;
  const RetainedRgbSession rotated(source, rotated_metadata);

  image::AssetMetadata canonical_metadata;
  canonical_metadata.raw_dimensions = {4U, 8U};
  canonical_metadata.image_dimensions = {4U, 8U};
  const RetainedRgbSession canonical(source, canonical_metadata);

  const std::array nodes{
      image::AdjustmentNode{
          .node_id = "orientation-aware-native-denoise",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters =
              image::SharpenAdjustment{
                  .denoise_luminance = 0.7,
                  .denoise_color = 0.3,
              },
      },
  };
  const auto rotated_proxy =
      image::prepare_warm_edit_preview(rotated, 8U).render_jpeg(nodes, 100U);
  const auto canonical_proxy =
      image::prepare_warm_edit_preview(canonical, 8U).render_jpeg(nodes, 100U);
  expect(rotated_proxy.bytes == canonical_proxy.bytes,
         "rotated RAW metadata uses the oriented full raster for native-radius "
         "effects");
}


void warm_edit_preview_bounds_fail_before_decode() {
  const FakeRgbSession session;
  try {
    static_cast<void>(image::prepare_warm_edit_preview(session, 0));
    expect(false, "zero warm edge must fail");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::invalid_request,
           "zero warm edge reports an invalid request");
  }
  try {
    static_cast<void>(image::prepare_warm_edit_preview(
        session, image::maximum_warm_edit_preview_edge + 1U));
    expect(false, "oversized warm edge must fail");
  } catch (const image::DecodeError &error) {
    expect(error.code() == image::DecodeErrorCode::invalid_request,
           "oversized warm edge reports an invalid request");
  }
  expect(session.reference_render_count() == 0U,
         "invalid warm bounds are rejected before decoder work");
}

void edited_proxy_rejects_invalid_nodes_before_decode() {
  const FakeRgbSession session;
  const image::ProxyRequest request{.max_edge = 4, .jpeg_quality = 90};

  const auto rejects_before_decode =
      [&](const image::AdjustmentNode &invalid_node,
          const image::EditErrorCode expected_code,
          const std::string_view message) {
        const std::array nodes{invalid_node};
        try {
          static_cast<void>(image::render_edited_reference_proxy_jpeg(
              session, nodes, request));
          expect(false, message);
        } catch (const image::EditError &error) {
          expect(error.code() == expected_code, message);
          expect(error.node_index() == 0U,
                 "preflight errors retain node provenance");
        }
        expect(session.reference_render_count() == 0U,
               "adjustment preflight rejects invalid nodes before rendering "
               "reference RGB");
      };

  rejects_before_decode(
      image::AdjustmentNode{
          .node_id = "non-finite-disabled-exposure",
          .enabled = false,
          .parameters =
              image::ExposureAdjustment{
                  std::numeric_limits<double>::quiet_NaN(),
              },
      },
      image::EditErrorCode::invalid_parameter,
      "preflight validates numeric parameters even on disabled nodes");

  rejects_before_decode(
      image::AdjustmentNode{
          .node_id = "future-version",
          .implementation_version =
              image::adjustment_implementation_version + 1U,
          .parameters = image::ExposureAdjustment{},
      },
      image::EditErrorCode::unsupported_version,
      "preflight rejects unsupported adjustment implementations");

  image::OklabLightnessToneCurve overflowing_slope;
  overflowing_slope.lightness.points = {
      {0.0, 0.0},
      {
          std::numeric_limits<double>::min(),
          std::numeric_limits<double>::max(),
      },
      {1.0, 1.0},
  };
  rejects_before_decode(
      image::AdjustmentNode{
          .node_id = "overflowing-tone-curve-slope",
          .parameters = std::move(overflowing_slope),
      },
      image::EditErrorCode::invalid_parameter,
      "preflight rejects non-finite Tone Curve segment slopes");
}

template <typename Render> [[nodiscard]] double median_render_ms(Render&& render) {
    constexpr std::size_t sample_count = 9U;
    std::vector<double> samples;
    samples.reserve(sample_count);
    std::uint64_t checksum = 0U;
    for (std::size_t sample = 0U; sample < sample_count; ++sample) {
        const auto start = std::chrono::steady_clock::now();
        const auto output = render();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        expect(!output.bytes.empty(), "benchmark render must produce pixels");
        checksum += output.bytes[sample % output.bytes.size()];
    }
    std::ranges::sort(samples);
    expect(checksum > 0U, "benchmark must consume the rendered output");
    return samples[samples.size() / 2U];
}

void benchmark_rgb8_transport_when_requested() {
    if (std::getenv("SHADOW_TEST_EDIT_PREVIEW_TRANSPORT_BENCHMARK") == nullptr) {
        return;
    }
    constexpr std::uint32_t width = 1'536U;
    constexpr std::uint32_t height = 1'024U;
    const RetainedRgbSession session(processed_linear_gradient(width, height));
    const auto warm = image::prepare_warm_edit_preview(session, width);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "benchmark-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.25},
        },
    };
    static_cast<void>(warm.render_rgb8(plan));
    static_cast<void>(warm.render_jpeg(plan, 90U));
    const double rgb_ms = median_render_ms([&] { return warm.render_rgb8(plan); });
    const double jpeg_ms = median_render_ms([&] { return warm.render_jpeg(plan, 90U); });
    std::cout << "BENCH preview-render " << width << 'x' << height << " rgb8-render-ms=" << rgb_ms
              << " jpeg-render-ms=" << jpeg_ms << " render-speedup=" << jpeg_ms / rgb_ms << "x\n";
}

} // namespace

int main() {
  raw_development_receipt_survives_prepared_edit_sessions();
  warm_edit_preview_decodes_once_and_renders_repeatedly();
  rotated_raw_preview_preserves_native_effect_radius();
  warm_edit_preview_bounds_fail_before_decode();
  edited_proxy_rejects_invalid_nodes_before_decode();
  benchmark_rgb8_transport_when_requested();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
