#include "detail_tile_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::reference_rgb;
using shadow::image::test_support::ScopedEnvironment;
using shadow::image::test_support::SyntheticDecodeSession;

void public_preview_geometry_stays_fused_on_metal() {
    constexpr image::Dimensions dimensions{160U, 112U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto warm = image::prepare_warm_edit_preview(decoder, dimensions.width);
    const image::PhotoGeometry geometry{
        .crop_left = 0.08,
        .crop_top = 0.1,
        .crop_right = 0.93,
        .crop_bottom = 0.9,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .straighten_degrees = 5.25,
        .flip_horizontal = true,
    };
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "geometry-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.21},
        },
        image::AdjustmentNode{
            .node_id = "geometry-saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.88},
        },
    };
    image::EncodedProxy cpu;
    {
        const ScopedEnvironment forced_cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
        cpu = warm.render_rgb8(nodes, geometry);
    }
    image::EncodedProxy gpu;
    {
        const ScopedEnvironment forced_metal("SHADOW_IMAGE_ACCELERATION", "metal");
        gpu = warm.render_rgb8(nodes, geometry);
    }
    std::uint8_t maximum_difference = 0U;
    if (cpu.bytes.size() == gpu.bytes.size()) {
        for (std::size_t index = 0U; index < cpu.bytes.size(); ++index) {
            const auto difference = static_cast<std::uint8_t>(
                std::max(cpu.bytes[index], gpu.bytes[index])
                - std::min(cpu.bytes[index], gpu.bytes[index])
            );
            maximum_difference = std::max(maximum_difference, difference);
        }
    }
    const auto expected = image::photo_geometry_layout(dimensions, geometry).output_dimensions;
    expect(
        gpu.dimensions == expected && gpu.format == image::PreviewFormat::bitmap
            && gpu.bytes.size() == static_cast<std::size_t>(expected.pixel_count()) * 3U,
        "public warm RGB8 geometry returns the authoritative transformed canvas"
    );
    expect(
        cpu.dimensions == gpu.dimensions && cpu.bytes.size() == gpu.bytes.size()
            && maximum_difference <= 1U,
        "public warm Metal geometry remains within one RGB8 level of the CPU route"
    );
    expect(
        warm.gpu_stats().completed_render_count >= 1U,
        "the public geometry request completes through the resident Metal session"
    );
    image::AnalyzedEditPreview analyzed;
    {
        const ScopedEnvironment forced_metal("SHADOW_IMAGE_ACCELERATION", "metal");
        analyzed = warm.render_jpeg_with_analysis(nodes, 90U, geometry);
    }
    expect(
        analyzed.proxy.dimensions == expected && analyzed.execution.valid()
            && analyzed.execution.adjustment_backend == image::EditPreviewBackend::metal
            && analyzed.execution.display_backend == image::EditPreviewBackend::metal
            && analyzed.execution.fused_pipeline && !analyzed.execution.adjustment_fell_back
            && !analyzed.execution.display_fell_back && analyzed.execution.diagnostic.empty(),
        "public analyzed geometry reports one complete fused Metal route"
    );
}

} // namespace

int main() {
    public_preview_geometry_stays_fused_on_metal();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
