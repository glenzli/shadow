#include "contract_test_assertions.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <span>
#include <stdexcept>
#include <string>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::ScopedEnvironment;


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


template <std::size_t Size>
[[nodiscard]] std::uint64_t
sum_counts(const std::array<std::uint64_t, Size> &values) {
  std::uint64_t sum = 0U;
  for (const auto value : values) {
    sum += value;
  }
  return sum;
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
                 "display=cpu-v1;display-contract=2;route=staged",
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
  image::EditPreviewExecutionReceipt presentation_fallback = fused;
  presentation_fallback.presentation_fell_back = true;
  presentation_fallback.diagnostic =
      "presentation: test-injected native texture failure";
  expect(
      presentation_fallback.valid() &&
          image::edit_preview_execution_receipt_identity(
              presentation_fallback) == fused_identity,
      "presentation-only fallback is valid and does not change the rendered "
      "pixel cache identity");
  image::EditPreviewExecutionReceipt impossible_cpu_presentation_fallback = cpu;
  impossible_cpu_presentation_fallback.presentation_fell_back = true;
  impossible_cpu_presentation_fallback.diagnostic = "impossible";
  expect(!impossible_cpu_presentation_fallback.valid(),
         "a CPU display route cannot claim native Metal presentation fallback");
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


} // namespace

int main() {
  warm_edit_preview_analysis_is_pre_jpeg_and_strictly_pre_clamp();
  warm_edit_preview_receipt_tracks_the_effective_display_backend();
  edit_preview_execution_identity_excludes_fallback_diagnostics();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
