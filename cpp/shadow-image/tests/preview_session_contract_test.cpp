#include "decoder_contract_test_support.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/decoder.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit.hpp>
#include <shadow/image/optics.hpp>
#include <shadow/image/raw_development.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::FakeOpticsProvider;
using shadow::image::test_support::FakeRgbSession;
using shadow::image::test_support::optics_reference_buffer;
using shadow::image::test_support::RetainedRgbSession;

class ScopedEnvironment final {
public:
  ScopedEnvironment(const std::string_view name, const std::string_view value)
      : name_(name) {
    if (const char *current = std::getenv(name_.c_str()); current != nullptr) {
      previous_ = current;
    }
#if defined(_WIN32)
    static_cast<void>(_putenv_s(name_.c_str(), std::string(value).c_str()));
#else
    static_cast<void>(setenv(name_.c_str(), std::string(value).c_str(), 1));
#endif
  }

  ~ScopedEnvironment() {
#if defined(_WIN32)
    static_cast<void>(_putenv_s(
        name_.c_str(), previous_.has_value() ? previous_->c_str() : ""));
#else
    if (previous_.has_value()) {
      static_cast<void>(setenv(name_.c_str(), previous_->c_str(), 1));
    } else {
      static_cast<void>(unsetenv(name_.c_str()));
    }
#endif
  }

  ScopedEnvironment(const ScopedEnvironment &) = delete;
  ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

[[nodiscard]] bool
jpeg_uses_444_chroma_sampling(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 4U || bytes[0] != 0xffU || bytes[1] != 0xd8U) {
    return false;
  }
  std::size_t offset = 2U;
  while (offset + 4U <= bytes.size()) {
    if (bytes[offset] != 0xffU) {
      return false;
    }
    while (offset < bytes.size() && bytes[offset] == 0xffU) {
      ++offset;
    }
    if (offset >= bytes.size()) {
      return false;
    }
    const std::uint8_t marker = bytes[offset++];
    if (marker == 0xd9U || marker == 0xdaU) {
      return false;
    }
    if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) {
      continue;
    }
    if (offset + 2U > bytes.size()) {
      return false;
    }
    const std::size_t length =
        (static_cast<std::size_t>(bytes[offset]) << 8U) | bytes[offset + 1U];
    if (length < 2U || offset + length > bytes.size()) {
      return false;
    }
    const bool start_of_frame = marker >= 0xc0U && marker <= 0xcfU &&
                                marker != 0xc4U && marker != 0xc8U &&
                                marker != 0xccU;
    if (start_of_frame) {
      if (length < 11U || bytes[offset + 7U] != 3U) {
        return false;
      }
      for (std::size_t component = 0U; component < 3U; ++component) {
        const std::size_t sampling = offset + 9U + component * 3U;
        if (sampling >= offset + length || bytes[sampling] != 0x11U) {
          return false;
        }
      }
      return true;
    }
    offset += length;
  }
  return false;
}

class BoundaryRgbSession final : public image::DecodeSession {
public:
  [[nodiscard]] const image::AssetMetadata &metadata() const noexcept override {
    return metadata_;
  }

  [[nodiscard]] const image::DecodeCapabilities &
  capabilities() const noexcept override {
    return capabilities_;
  }

  [[nodiscard]] std::span<const image::PreviewDescriptor>
  previews() const noexcept override {
    return {};
  }

  [[nodiscard]] image::PreviewPayload decode_preview(std::size_t) override {
    throw image::DecodeError(image::DecodeErrorCode::no_preview, 0,
                             "no preview");
  }

  [[nodiscard]] image::RawFrame decode_raw_frame() override {
    throw image::DecodeError(image::DecodeErrorCode::unsupported, 0,
                             "no RAW frame");
  }

  [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
    ++reference_render_count_;
    return image::PixelBuffer{
        .dimensions = {5, 1},
        .bits_per_channel = 16,
        .channels = 3,
        .row_stride_bytes = 5U * 3U * sizeof(std::uint16_t),
        .primaries = image::RgbPrimaries::srgb_rec709_d65,
        .transfer_function = image::RgbTransferFunction::linear,
        .reference = image::RgbBufferReference::processed_raw,
        .samples =
            {
                0U,
                0U,
                0U,
                65'535U,
                65'535U,
                65'535U,
                65'535U,
                0U,
                0U,
                0U,
                65'535U,
                0U,
                0U,
                0U,
                65'535U,
            },
    };
  }

  [[nodiscard]] std::size_t reference_render_count() const noexcept {
    return reference_render_count_;
  }

private:
  image::AssetMetadata metadata_;
  image::DecodeCapabilities capabilities_;
  mutable std::size_t reference_render_count_ = 0;
};

void raw_development_receipt_survives_prepared_edit_sessions() {
  auto source = optics_reference_buffer(8U, 4U);
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

  const FakeOpticsProvider discarding_optics;
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

template <std::size_t Size>
[[nodiscard]] std::uint64_t
sum_counts(const std::array<std::uint64_t, Size> &values) {
  std::uint64_t sum = 0U;
  for (const auto value : values) {
    sum += value;
  }
  return sum;
}

void reference_proxy_is_bounded_standard_jpeg() {
  expect(image::proxy_dimensions({4'032, 3'024}, 2'048) ==
             image::Dimensions{2'048, 1'536},
         "proxy dimensions preserve aspect ratio and max edge");

  const FakeRgbSession session;
  const auto proxy = image::render_reference_proxy_jpeg(
      session, image::ProxyRequest{.max_edge = 4, .jpeg_quality = 88});
  expect(proxy.dimensions == image::Dimensions{4, 2},
         "proxy renderer downsizes RGB");
  expect(proxy.format == image::PreviewFormat::jpeg, "proxy output is JPEG");
  expect(proxy.bytes.size() > 4U, "proxy JPEG is not empty");
  expect(proxy.bytes[0] == 0xffU && proxy.bytes[1] == 0xd8U,
         "proxy output starts with JPEG SOI");
  expect(proxy.bytes[proxy.bytes.size() - 2U] == 0xffU &&
             proxy.bytes.back() == 0xd9U,
         "proxy output ends with JPEG EOI");
  expect(jpeg_uses_444_chroma_sampling(proxy.bytes),
         "interactive/reference JPEG proxies preserve 4:4:4 chroma sampling");
}

void dng_baseline_exposure_is_a_consistent_source_rendering_step() {
  const image::PixelBuffer source{
      .dimensions = {2U, 1U},
      .bits_per_channel = 16U,
      .channels = 3U,
      .row_stride_bytes = 2U * 3U * sizeof(std::uint16_t),
      .primaries = image::RgbPrimaries::srgb_rec709_d65,
      .transfer_function = image::RgbTransferFunction::linear,
      .reference = image::RgbBufferReference::processed_raw,
      .samples =
          {
              49'152U,
              49'152U,
              49'152U,
              57'344U,
              53'248U,
              49'152U,
          },
  };
  const image::ProxyRequest request{.max_edge = 2U, .jpeg_quality = 100U};
  const std::array<image::AdjustmentNode, 0U> no_nodes{};

  const RetainedRgbSession no_baseline(source);
  const auto neutral_proxy =
      image::render_reference_proxy_jpeg(no_baseline, request);

  image::AssetMetadata dng_metadata;
  dng_metadata.dng_version = "1.6.0.0";
  dng_metadata.baseline_exposure = 1.0;
  const RetainedRgbSession dng_source(source, dng_metadata);
  const auto dng_proxy =
      image::render_reference_proxy_jpeg(dng_source, request);
  expect(dng_proxy.bytes != neutral_proxy.bytes,
         "a valid DNG BaselineExposure changes the source rendering before "
         "display encoding");
  const auto dng_warm_proxy =
      image::render_edited_reference_proxy_jpeg(dng_source, no_nodes, request);
  expect(dng_warm_proxy.bytes == dng_proxy.bytes,
         "warm edit preview and the unedited DNG proxy share the baseline "
         "source rendering");

  const auto neutral_detail =
      image::prepare_full_edit_detail(no_baseline)
          .render_rgb8(no_nodes,
                       image::DetailTileRect{
                           .x = 0U, .y = 0U, .width = 2U, .height = 1U});
  const auto dng_detail =
      image::prepare_full_edit_detail(dng_source)
          .render_rgb8(no_nodes,
                       image::DetailTileRect{
                           .x = 0U, .y = 0U, .width = 2U, .height = 1U});
  expect(dng_detail.bytes != neutral_detail.bytes,
         "full-detail tiles apply the same DNG source baseline before the edit "
         "graph");

  auto invalid_dng_metadata = dng_metadata;
  invalid_dng_metadata.baseline_exposure = -999.0;
  const RetainedRgbSession missing_tag_sentinel(source, invalid_dng_metadata);
  expect(
      image::render_reference_proxy_jpeg(missing_tag_sentinel, request).bytes ==
          neutral_proxy.bytes,
      "LibRaw's absent-DNG-BaselineExposure sentinel is ignored");

  auto non_dng_metadata = dng_metadata;
  non_dng_metadata.dng_version.clear();
  const RetainedRgbSession non_dng_source(source, non_dng_metadata);
  expect(image::render_reference_proxy_jpeg(non_dng_source, request).bytes ==
             neutral_proxy.bytes,
         "non-DNG RAW files never inherit a guessed DNG baseline exposure");
}

void edited_proxy_applies_one_explicit_display_srgb_boundary() {
  const FakeRgbSession session;
  const image::ProxyRequest request{.max_edge = 8, .jpeg_quality = 90};
  const auto reference = image::render_reference_proxy_jpeg(session, request);
  const std::array neutral_nodes{
      image::AdjustmentNode{
          .node_id = "exposure",
          .parameters = image::ExposureAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "contrast",
          .parameters = image::ContrastAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "tone-curve",
          .parameters = image::OklabLightnessToneCurve{},
      },
      image::AdjustmentNode{
          .node_id = "rgb-white-balance",
          .parameters = image::RgbWhiteBalanceAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "saturation",
          .parameters = image::SaturationAdjustment{},
      },
  };
  const auto neutral = image::render_edited_reference_proxy_jpeg(
      session, neutral_nodes, request);
  expect(neutral.bytes == reference.bytes,
         "neutral edits share the one display-sRGB output transform with the "
         "reference path");

  auto adjusted_nodes = neutral_nodes;
  adjusted_nodes[0].parameters = image::ExposureAdjustment{1.0};
  adjusted_nodes[3].parameters = image::RgbWhiteBalanceAdjustment{
      .temperature = 0.2,
      .tint = -0.05,
  };
  const image::ProxyRequest small_request{.max_edge = 4, .jpeg_quality = 90};
  const auto neutral_small = image::render_edited_reference_proxy_jpeg(
      session, neutral_nodes, small_request);
  const auto adjusted = image::render_edited_reference_proxy_jpeg(
      session, adjusted_nodes, small_request);
  expect(adjusted.dimensions == image::Dimensions{4, 2},
         "edited preview remains bounded");
  expect(adjusted.bytes != neutral_small.bytes,
         "ordered edit nodes affect the encoded result");
  expect(adjusted.bytes.size() > 4U && adjusted.bytes[0] == 0xffU &&
             adjusted.bytes[1] == 0xd8U &&
             adjusted.bytes[adjusted.bytes.size() - 2U] == 0xffU &&
             adjusted.bytes.back() == 0xd9U,
         "edited preview is a standard JPEG");
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
  expect(session.reference_render_count() == 1U,
         "repeated warm renders never ask the decoder for pixels again");
  expect(neutral.dimensions == image::Dimensions{4, 2},
         "warm output dimensions stay fixed");
  expect(adjusted.dimensions == neutral.dimensions,
         "all warm renders share working dimensions");
  expect(adjusted.bytes != neutral.bytes,
         "warm renders apply each requested edit independently");

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
  const auto source = optics_reference_buffer(4U, 8U);
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

void warm_edit_preview_analysis_is_pre_jpeg_and_strictly_pre_clamp() {
  const ScopedEnvironment forced_cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
  const BoundaryRgbSession session;
  const auto warm = image::prepare_warm_edit_preview(session, 5);
  expect(session.reference_render_count() == 1U,
         "analysis preparation decodes exactly once");

  const std::array neutral_nodes{
      image::AdjustmentNode{
          .node_id = "neutral-exposure",
          .parameters = image::ExposureAdjustment{},
      },
  };
  const auto low_quality = warm.render_jpeg_with_analysis(neutral_nodes, 1);
  const auto high_quality = warm.render_jpeg_with_analysis(neutral_nodes, 100);
  const auto &neutral = low_quality.analysis;

  expect(neutral == high_quality.analysis,
         "JPEG quality cannot affect analysis computed from pre-encode RGB8");
  expect(
      low_quality.proxy.bytes != high_quality.proxy.bytes,
      "the quality-independence check still exercises distinct JPEG encodings");
  expect(low_quality.execution.valid() &&
             low_quality.execution.adjustment_backend ==
                 image::EditPreviewBackend::cpu &&
             low_quality.execution.display_backend ==
                 image::EditPreviewBackend::cpu &&
             image::edit_preview_execution_receipt_identity(
                 low_quality.execution) ==
                 image::edit_preview_execution_receipt_identity(
                     high_quality.execution),
         "one completed warm render reports its effective CPU adjustment and "
         "display route");
  expect(neutral.sample_dimensions == image::Dimensions{5, 1} &&
             neutral.pixel_count == 5U,
         "analysis describes the complete warm proxy");
  expect(
      neutral.luma[0] >= 1U,
      "fixed-point Rec.709 encoded luma retains the black analysis endpoint");
  expect(sum_counts(neutral.red) == neutral.pixel_count &&
             sum_counts(neutral.green) == neutral.pixel_count &&
             sum_counts(neutral.blue) == neutral.pixel_count &&
             sum_counts(neutral.luma) == neutral.pixel_count,
         "every histogram contains exactly one sample per proxy pixel");
  expect(neutral.below_zero_samples ==
                 std::array<std::uint64_t, 3>{0U, 0U, 0U} &&
             neutral.above_one_samples ==
                 std::array<std::uint64_t, 3>{0U, 0U, 0U} &&
             neutral.shadow_clipped_pixels == 0U &&
             neutral.highlight_clipped_pixels == 0U,
         "exact scene-linear zero and one are legal and are not reported as "
         "clipped");

  const std::array highlight_nodes{
      image::AdjustmentNode{
          .node_id = "highlight-exposure",
          .parameters = image::ExposureAdjustment{1.0},
      },
  };
  const auto highlight =
      warm.render_jpeg_with_analysis(highlight_nodes, 80).analysis;
  expect(highlight.above_one_samples ==
                 std::array<std::uint64_t, 3>{2U, 2U, 2U} &&
             highlight.highlight_clipped_pixels == 4U,
         "super-white channels and their any-channel pixel union are counted "
         "independently");
  expect(highlight.hdr_headroom_pixels == 2U &&
             sum_counts(highlight.hdr_headroom_bins) ==
                 highlight.hdr_headroom_pixels &&
             highlight.hdr_headroom_bins[0] == 1U &&
             highlight.hdr_headroom_bins[1] == 1U &&
             std::abs(highlight.hdr_peak_headroom_ev - 1.0) < 1.0e-6,
         "HDR headroom measures pre-SDR linear luminance rather than "
         "any-channel clipping");

  image::OklabLightnessToneCurve lowered_curve;
  lowered_curve.lightness.points = {{0.0, -0.1}, {1.0, 0.9}};
  const std::array shadow_nodes{
      image::AdjustmentNode{
          .node_id = "lowered-curve",
          .parameters = std::move(lowered_curve),
      },
  };
  const auto shadow = warm.render_jpeg_with_analysis(shadow_nodes, 80).analysis;
  expect(shadow.below_zero_samples ==
                 std::array<std::uint64_t, 3>{2U, 3U, 3U} &&
             shadow.shadow_clipped_pixels == 4U,
         "Oklab-lightness shadows retain chroma while per-channel and pixel "
         "clipping stay distinct");
  expect(session.reference_render_count() == 1U,
         "repeated analyzed renders never ask the decoder for pixels again");

  auto first_concurrent =
      std::async(std::launch::async, [&warm, &neutral_nodes]() {
        return warm.render_jpeg_with_analysis(neutral_nodes, 80);
      });
  auto second_concurrent =
      std::async(std::launch::async, [&warm, &neutral_nodes]() {
        return warm.render_jpeg_with_analysis(neutral_nodes, 80);
      });
  const auto first_result = first_concurrent.get();
  const auto second_result = second_concurrent.get();
  expect(first_result.analysis == second_result.analysis &&
             first_result.proxy.bytes == second_result.proxy.bytes &&
             image::edit_preview_execution_receipt_identity(
                 first_result.execution) ==
                 image::edit_preview_execution_receipt_identity(
                     second_result.execution),
         "concurrent const analyzed renders are deterministic and isolated");
}

void warm_edit_preview_receipt_tracks_the_effective_display_backend() {
  const BoundaryRgbSession session;
  const auto warm = image::prepare_warm_edit_preview(session, 5U);
  const auto resident_before_render = warm.gpu_stats();
  if (resident_before_render.resident) {
    expect(resident_before_render.source_upload_count == 1U &&
               resident_before_render.gpu_buffer_allocation_count == 10U &&
               resident_before_render.render_count == 0U &&
               resident_before_render.completed_render_count == 0U &&
               resident_before_render.curve_resource_upload_count == 0U &&
               resident_before_render.lut_resource_upload_count == 0U &&
               resident_before_render.resource_cache_hit_count == 0U &&
               resident_before_render.resident_bytes > 0U,
           "warm Metal preparation uploads one immutable source, an empty side "
           "table, "
           "and two fixed slots");
  }
  const std::array neutral_nodes{
      image::AdjustmentNode{
          .node_id = "receipt-neutral-exposure",
          .parameters = image::ExposureAdjustment{},
      },
  };

  image::AnalyzedEditPreview cpu;
  {
    const ScopedEnvironment forced_cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
    cpu = warm.render_jpeg_with_analysis(neutral_nodes, 90U);
  }
  expect(
      cpu.execution.valid() &&
          cpu.execution.adjustment_backend == image::EditPreviewBackend::cpu &&
          cpu.execution.adjustment_backend_version ==
              image::edit_preview_cpu_adjustment_backend_version &&
          cpu.execution.display_backend == image::EditPreviewBackend::cpu &&
          cpu.execution.display_backend_version ==
              image::edit_preview_cpu_display_backend_version &&
          !cpu.execution.adjustment_fell_back &&
          !cpu.execution.display_fell_back && cpu.execution.diagnostic.empty(),
      "forced CPU warm preview reports CPU adjustment/display without "
      "fallback");

  if (resident_before_render.resident) {
    image::AnalyzedEditPreview metal;
    {
      const ScopedEnvironment forced_metal("SHADOW_IMAGE_ACCELERATION",
                                           "metal");
      metal = warm.render_jpeg_with_analysis(neutral_nodes, 90U);
    }
    expect(metal.execution.valid() &&
               metal.execution.adjustment_backend ==
                   image::EditPreviewBackend::cpu &&
               metal.execution.display_backend ==
                   image::EditPreviewBackend::metal &&
               metal.execution.display_backend_version ==
                   image::edit_preview_metal_display_backend_version &&
               metal.execution.fused_pipeline &&
               !metal.execution.adjustment_fell_back &&
               !metal.execution.display_fell_back &&
               metal.execution.diagnostic.empty(),
           "forced Metal warm preview keeps adjustment on CPU and reports "
           "Metal display");
    expect(image::edit_preview_execution_receipt_identity(metal.execution) !=
               image::edit_preview_execution_receipt_identity(cpu.execution),
           "effective Metal display output cannot reuse a CPU warm-preview "
           "identity");
    const std::array active_nodes{
        image::AdjustmentNode{
            .node_id = "receipt-active-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.25},
        },
    };
    const auto accelerated = warm.render_jpeg_with_analysis(active_nodes, 90U);
    expect(accelerated.execution.valid() &&
               accelerated.execution.adjustment_backend ==
                   image::EditPreviewBackend::metal &&
               accelerated.execution.adjustment_backend_version ==
                   image::edit_preview_metal_adjustment_backend_version &&
               accelerated.execution.display_backend ==
                   image::EditPreviewBackend::metal &&
               accelerated.execution.display_backend_version ==
                   image::edit_preview_metal_display_backend_version &&
               accelerated.execution.fused_pipeline &&
               !accelerated.execution.adjustment_fell_back &&
               !accelerated.execution.display_fell_back &&
               accelerated.execution.diagnostic.empty(),
           "an active supported warm-preview plan reports Metal adjustment and "
           "display");

    const auto allocation_snapshot = warm.gpu_stats();
    const auto interactive_a = warm.render_jpeg(active_nodes, 84U);
    const auto interactive_b = warm.render_jpeg(active_nodes, 84U);
    const auto after_repeated_render = warm.gpu_stats();
    expect(interactive_a.bytes == interactive_b.bytes &&
               after_repeated_render.source_upload_count ==
                   allocation_snapshot.source_upload_count &&
               after_repeated_render.gpu_buffer_allocation_count ==
                   allocation_snapshot.gpu_buffer_allocation_count &&
               after_repeated_render.render_count ==
                   allocation_snapshot.render_count + 2U &&
               after_repeated_render.completed_render_count ==
                   allocation_snapshot.completed_render_count + 2U,
           "interactive warm renders reuse one source upload and two "
           "preallocated GPU slots");

    const auto before_concurrent = warm.gpu_stats();
    auto first_concurrent =
        std::async(std::launch::async, [&warm, &active_nodes]() {
          return warm.render_jpeg_with_analysis(active_nodes, 88U);
        });
    auto second_concurrent =
        std::async(std::launch::async, [&warm, &active_nodes]() {
          return warm.render_jpeg_with_analysis(active_nodes, 88U);
        });
    const auto first_concurrent_result = first_concurrent.get();
    const auto second_concurrent_result = second_concurrent.get();
    const auto after_concurrent = warm.gpu_stats();
    expect(first_concurrent_result.analysis ==
                   second_concurrent_result.analysis &&
               first_concurrent_result.proxy.bytes ==
                   second_concurrent_result.proxy.bytes &&
               after_concurrent.source_upload_count ==
                   before_concurrent.source_upload_count &&
               after_concurrent.gpu_buffer_allocation_count ==
                   before_concurrent.gpu_buffer_allocation_count &&
               after_concurrent.render_count ==
                   before_concurrent.render_count + 2U &&
               after_concurrent.completed_render_count ==
                   before_concurrent.completed_render_count + 2U &&
               after_concurrent.peak_concurrent_renders >= 1U &&
               after_concurrent.peak_concurrent_renders <= 2U,
           "concurrent fused renders are deterministic and bounded by two "
           "reusable slots");

    image::AnalyzedEditPreview automatic_fallback;
    {
      const ScopedEnvironment injected_failure(
          "SHADOW_TEST_WARM_METAL_FORCE_FAILURE", "1");
      automatic_fallback = warm.render_jpeg_with_analysis(active_nodes, 90U);
    }
    expect(automatic_fallback.execution.valid() &&
               automatic_fallback.execution.adjustment_backend ==
                   image::EditPreviewBackend::cpu &&
               automatic_fallback.execution.display_backend ==
                   image::EditPreviewBackend::cpu &&
               automatic_fallback.execution.adjustment_fell_back &&
               automatic_fallback.execution.display_fell_back &&
               !automatic_fallback.execution.diagnostic.empty(),
           "a failed fused attempt replays adjustment and display completely "
           "on CPU");
    try {
      const ScopedEnvironment forced_metal("SHADOW_IMAGE_ACCELERATION",
                                           "metal");
      const ScopedEnvironment injected_failure(
          "SHADOW_TEST_WARM_METAL_FORCE_FAILURE", "1");
      static_cast<void>(warm.render_jpeg(active_nodes, 90U));
      expect(false,
             "forced Metal does not silently replay a failed warm render");
    } catch (const image::EditError &error) {
      expect(error.code() == image::EditErrorCode::backend_failure,
             "forced warm Metal failure preserves typed backend semantics");
    }
  } else {
    expect(std::getenv("SHADOW_TEST_REQUIRE_WARM_METAL") == nullptr,
           "session-resident warm Metal was required but is unavailable");
    image::AnalyzedEditPreview fallback;
    {
      const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
      fallback = warm.render_jpeg_with_analysis(neutral_nodes, 90U);
    }
    expect(fallback.execution.valid() &&
               fallback.execution.adjustment_backend ==
                   image::EditPreviewBackend::cpu &&
               fallback.execution.display_backend ==
                   image::EditPreviewBackend::cpu &&
               !fallback.execution.adjustment_fell_back &&
               fallback.execution.display_fell_back &&
               !fallback.execution.diagnostic.empty(),
           "CPU-only automatic warm preview exposes its Metal display fallback "
           "diagnostic");
    expect(
        image::edit_preview_execution_receipt_identity(fallback.execution) ==
                image::edit_preview_execution_receipt_identity(cpu.execution) &&
            fallback.execution.diagnostic != cpu.execution.diagnostic,
        "fallback diagnostics stay outside the effective CPU cache identity");
  }

  const std::string generator =
      image::edit_preview_generator_implementation_identity();
  expect(generator.find(std::string(image::display_output_backend_identity(
             image::DisplayOutputBackend::cpu))) != std::string::npos &&
             generator.find(std::string(image::display_output_backend_identity(
                 image::DisplayOutputBackend::metal))) != std::string::npos,
         "generator identity names both selectable display-output "
         "implementations");
  expect(generator.find(std::string(image::adjustment_backend_identity(
             image::AdjustmentBackend::cpu))) != std::string::npos &&
             generator.find(std::string(image::adjustment_backend_identity(
                 image::AdjustmentBackend::metal))) != std::string::npos,
         "generator identity names both selectable adjustment implementations");
}

void edit_preview_execution_identity_excludes_fallback_diagnostics() {
  image::EditPreviewExecutionReceipt cpu;
  const std::string cpu_identity =
      image::edit_preview_execution_receipt_identity(cpu);
  expect(cpu.valid() &&
             cpu_identity ==
                 "shadow-edit-preview-execution-v1;adjustment=cpu-v1;plan=1;"
                 "display=cpu-v1;display-contract=1;route=staged",
         "the current CPU adjustment/display route has one canonical cache "
         "identity");

  image::EditPreviewExecutionReceipt fallback = cpu;
  fallback.adjustment_fell_back = true;
  fallback.display_fell_back = true;
  fallback.diagnostic = "/Users/example/private/device diagnostic";
  expect(image::edit_preview_execution_receipt_identity(fallback) ==
                 cpu_identity &&
             cpu_identity.find("/Users") == std::string::npos,
         "fallback diagnostics and user-local paths never participate in "
         "canonical identity");

  image::EditPreviewExecutionReceipt metal_adjustment = cpu;
  metal_adjustment.adjustment_backend = image::EditPreviewBackend::metal;
  metal_adjustment.adjustment_backend_version =
      image::edit_preview_metal_adjustment_backend_version;
  expect(image::edit_preview_execution_receipt_identity(metal_adjustment) !=
             cpu_identity,
         "an effective Metal adjustment route cannot reuse a CPU preview cache "
         "entry");

  image::EditPreviewExecutionReceipt metal_display = cpu;
  metal_display.display_backend = image::EditPreviewBackend::metal;
  metal_display.display_backend_version =
      image::edit_preview_metal_display_backend_version;
  expect(image::edit_preview_execution_receipt_identity(metal_display) !=
             cpu_identity,
         "an effective Metal display route cannot reuse a CPU display cache "
         "entry");

  image::EditPreviewExecutionReceipt fused = cpu;
  fused.adjustment_backend = image::EditPreviewBackend::metal;
  fused.adjustment_backend_version =
      image::edit_preview_metal_adjustment_backend_version;
  fused.display_backend = image::EditPreviewBackend::metal;
  fused.display_backend_version =
      image::edit_preview_metal_display_backend_version;
  fused.fused_pipeline = true;
  const std::string fused_identity =
      image::edit_preview_execution_receipt_identity(fused);
  expect(fused.valid() && fused_identity != cpu_identity &&
             fused_identity != image::edit_preview_execution_receipt_identity(
                                   metal_adjustment),
         "session-resident fused Metal has a distinct cache-safe execution "
         "identity");
  image::EditPreviewExecutionReceipt impossible_fused_hybrid = fused;
  impossible_fused_hybrid.display_backend = image::EditPreviewBackend::cpu;
  impossible_fused_hybrid.display_backend_version =
      image::edit_preview_cpu_display_backend_version;
  expect(!impossible_fused_hybrid.valid(),
         "a fused adjustment receipt cannot masquerade as the split display "
         "stage");
  expect(image::edit_preview_generator_implementation_identity().find(
             "shadow-edit-preview-generator-v1") != std::string::npos &&
             image::edit_preview_generator_implementation_identity().find(
                 "/Users") == std::string::npos,
         "the pre-render generator identity is implementation-only and cache "
         "safe");

  image::EditPreviewExecutionReceipt stale = cpu;
  ++stale.schema_version;
  expect(!stale.valid(), "unknown execution receipt schemas fail closed");
  try {
    static_cast<void>(image::edit_preview_execution_receipt_identity(stale));
    expect(false,
           "an invalid execution receipt cannot produce a cache identity");
  } catch (const std::invalid_argument &) {
    // Expected.
  }

  image::EditPreviewExecutionReceipt stale_adjustment_backend = cpu;
  ++stale_adjustment_backend.adjustment_backend_version;
  expect(
      !stale_adjustment_backend.valid(),
      "a stale CPU adjustment backend version fails the C++ receipt contract");
  image::EditPreviewExecutionReceipt stale_display_backend = cpu;
  ++stale_display_backend.display_backend_version;
  expect(!stale_display_backend.valid(),
         "a stale CPU display backend version fails the C++ receipt contract");
  image::EditPreviewExecutionReceipt impossible_adjustment_fallback = cpu;
  impossible_adjustment_fallback.adjustment_backend =
      image::EditPreviewBackend::metal;
  impossible_adjustment_fallback.adjustment_backend_version =
      image::edit_preview_metal_adjustment_backend_version;
  impossible_adjustment_fallback.adjustment_fell_back = true;
  impossible_adjustment_fallback.diagnostic = "impossible";
  expect(!impossible_adjustment_fallback.valid(),
         "a Metal backend cannot claim that it fell back");
  image::EditPreviewExecutionReceipt missing_fallback_diagnostic = cpu;
  missing_fallback_diagnostic.adjustment_fell_back = true;
  expect(!missing_fallback_diagnostic.valid(),
         "a fallback receipt requires a diagnostic");
  image::EditPreviewExecutionReceipt stray_diagnostic = cpu;
  stray_diagnostic.diagnostic = "stray";
  expect(!stray_diagnostic.valid(),
         "a non-fallback receipt cannot carry a fallback diagnostic");
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

} // namespace

int main() {
  raw_development_receipt_survives_prepared_edit_sessions();
  reference_proxy_is_bounded_standard_jpeg();
  dng_baseline_exposure_is_a_consistent_source_rendering_step();
  edited_proxy_applies_one_explicit_display_srgb_boundary();
  warm_edit_preview_decodes_once_and_renders_repeatedly();
  rotated_raw_preview_preserves_native_effect_radius();
  warm_edit_preview_analysis_is_pre_jpeg_and_strictly_pre_clamp();
  warm_edit_preview_receipt_tracks_the_effective_display_backend();
  edit_preview_execution_identity_excludes_fallback_diagnostics();
  warm_edit_preview_bounds_fail_before_decode();
  edited_proxy_rejects_invalid_nodes_before_decode();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
