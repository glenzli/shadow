#include "detail_tile_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/full_edit_detail.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::reference_rgb;
using shadow::image::test_support::ScopedEnvironment;
using shadow::image::test_support::SyntheticDecodeSession;

void continuous_clone_crosses_irregular_detail_tiles_without_seams() {
    constexpr image::Dimensions dimensions{128U, 80U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", "auto");
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "continuous-clone-across-tiles",
            .parameters = image::SpotHealAdjustment{
                .strokes = {{
                    .points = {
                        {.x = 0.16, .y = 0.28},
                        {.x = 0.39, .y = 0.47},
                        {.x = 0.63, .y = 0.51},
                        {.x = 0.84, .y = 0.72},
                    },
                    .radius_level_zero_pixels = 5U,
                    .mode = image::SpotRepairMode::clone,
                    .source_offset_x_radii = 2.0,
                    .source_offset_y_radii = -1.0,
                    .feather = 0.28,
                }},
            },
        },
    };
    const auto full =
        session.render_rgb8(plan, {0U, 0U, dimensions.width, dimensions.height});
    bool all_used_metal = full.execution.backend == image::DetailTileRenderBackend::metal;
    std::vector<std::uint8_t> stitched(full.bytes.size(), 0U);
    constexpr std::array tiles{
        image::DetailTileRect{0U, 0U, 43U, 31U},
        image::DetailTileRect{43U, 0U, 37U, 31U},
        image::DetailTileRect{80U, 0U, 48U, 31U},
        image::DetailTileRect{0U, 31U, 43U, 49U},
        image::DetailTileRect{43U, 31U, 37U, 49U},
        image::DetailTileRect{80U, 31U, 48U, 49U},
    };
    for (const image::DetailTileRect rect : tiles) {
        const auto tile = session.render_rgb8(plan, rect);
        all_used_metal =
            all_used_metal && tile.execution.backend == image::DetailTileRenderBackend::metal;
        for (std::uint32_t row = 0U; row < rect.height; ++row) {
            const auto source =
                tile.bytes.cbegin() + static_cast<std::ptrdiff_t>(row * tile.row_stride_bytes);
            const std::size_t destination =
                (static_cast<std::size_t>(rect.y + row) * dimensions.width + rect.x) * 3U;
            std::copy_n(
                source,
                static_cast<std::ptrdiff_t>(tile.row_stride_bytes),
                stitched.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    expect(
        stitched == full.bytes,
        "continuous Clone produces identical full-frame and donor-apron tile output"
    );
    expect(
        !image::adjustment_backend_available(image::AdjustmentBackend::metal)
            || all_used_metal,
        "the full-resolution continuous Clone seam contract executes on resident Metal"
    );
}

} // namespace

int main() {
    continuous_clone_crosses_irregular_detail_tiles_without_seams();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
