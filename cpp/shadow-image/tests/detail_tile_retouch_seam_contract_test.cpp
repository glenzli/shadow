#include "detail_tile_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::reference_rgb;
using shadow::image::test_support::ScopedEnvironment;
using shadow::image::test_support::SyntheticDecodeSession;

void continuous_clone_crosses_irregular_detail_tiles_without_seams(const char* acceleration) {
    constexpr image::Dimensions dimensions{128U, 80U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto session = image::prepare_full_edit_detail(decoder);
    const ScopedEnvironment automatic("SHADOW_IMAGE_ACCELERATION", acceleration);
    const std::array plan{
        image::AdjustmentNode{
            .node_id = "continuous-clone-across-tiles",
            .parameters = image::SpotHealAdjustment{
                .spots = {{
                    .center_x = 0.50,
                    .center_y = 0.39,
                    .radius_level_zero_pixels = 6U,
                    .mode = image::SpotRepairMode::heal,
                    .source_offset_x_radii = 2.5,
                    .source_offset_y_radii = 1.0,
                    .feather = 0.22,
                }},
                .strokes = {{
                    .points =
                        {
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
    const auto full = session.render_rgb8(plan, {0U, 0U, dimensions.width, dimensions.height});
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
        "ordered Heal and continuous Clone produce identical full-frame and donor-apron tile output"
    );
    if (stitched != full.bytes) {
        std::size_t differences = 0U;
        unsigned maximum_delta = 0U;
        for (std::size_t index = 0U; index < stitched.size(); ++index) {
            const auto delta = static_cast<unsigned>(
                std::abs(static_cast<int>(stitched[index]) - static_cast<int>(full.bytes[index]))
            );
            if (delta == 0U) {
                continue;
            }
            if (differences < 8U) {
                std::cerr << "tile mismatch x=" << index / 3U % dimensions.width
                          << " y=" << index / 3U / dimensions.width << " channel=" << index % 3U
                          << " delta=" << delta << '\n';
            }
            ++differences;
            maximum_delta = std::max(maximum_delta, delta);
        }
        std::cerr << acceleration << " tile differing channels=" << differences
                  << " maximum delta=" << maximum_delta << '\n';
    }
    expect(
        std::string_view(acceleration) == "cpu"
            || !image::adjustment_backend_available(image::AdjustmentBackend::metal)
            || all_used_metal,
        "the full-resolution Heal/Clone seam contract executes on resident Metal"
    );
}

} // namespace

int main() {
    expect(
        image::edit_preview_generator_implementation_identity().find(
            ";warm-retouch-donor=20260920.1;"
        ) != std::string::npos,
        "the corrected donor-coordinate math invalidates durable edited previews"
    );
    continuous_clone_crosses_irregular_detail_tiles_without_seams("cpu");
    continuous_clone_crosses_irregular_detail_tiles_without_seams("auto");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
