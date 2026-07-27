#include "contract_test_assertions.hpp"
#include "processed_rgb_session_fixture.hpp"

#include <shadow/image/edited_proxy_rendering.hpp>
#include <shadow/image/proxy_rendering.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::FakeRgbSession;


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


} // namespace

int main() {
  reference_proxy_is_bounded_standard_jpeg();
  edited_proxy_applies_one_explicit_display_srgb_boundary();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
