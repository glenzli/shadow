#include "contract_test_assertions.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/edit.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/reference_pixels.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <stop_token>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

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
  jpeg_raster_provider_uses_the_common_non_destructive_graph();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
