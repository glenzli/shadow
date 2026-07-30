#include "detail_tile_contract_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::metadata;
using shadow::image::test_support::reference_rgb;
using shadow::image::test_support::ScopedEnvironment;
using shadow::image::test_support::SyntheticDecodeSession;

void public_preview_liquify_falls_back_atomically_to_the_cpu_sampler() {
    constexpr image::Dimensions dimensions{160U, 112U};
    SyntheticDecodeSession decoder(metadata(dimensions), reference_rgb(dimensions));
    const auto warm = image::prepare_warm_edit_preview(decoder, dimensions.width);
    const image::PhotoGeometry geometry{
        .crop_left = 0.08,
        .crop_top = 0.1,
        .crop_right = 0.93,
        .crop_bottom = 0.9,
        .quarter_turn = image::PhotoQuarterTurn::clockwise_90,
        .straighten_degrees = 2.5,
    };
    const image::PhotoLiquify liquify{
        .strokes =
            {
                image::PhotoLiquifyPushStroke{
                    .points =
                        {
                            {.x = 0.33, .y = 0.48, .pressure = 1.0},
                            {.x = 0.58, .y = 0.52, .pressure = 0.7},
                        },
                    .radius = 0.16,
                    .strength = 0.4,
                    .hardness = 0.6,
                },
            },
    };
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "liquify-exposure",
            .parameters = image::ExposureAdjustment{.stops = 0.21},
        },
    };
    image::EncodedProxy cpu;
    {
        const ScopedEnvironment forced_cpu("SHADOW_IMAGE_ACCELERATION", "cpu");
        cpu = warm.render_rgb8(nodes, geometry, &liquify);
    }
    image::EncodedProxy automatic;
    image::AnalyzedEditPreview analyzed;
    {
        const ScopedEnvironment auto_backend("SHADOW_IMAGE_ACCELERATION", "auto");
        automatic = warm.render_rgb8(nodes, geometry, &liquify);
        analyzed = warm.render_jpeg_with_analysis(nodes, 90U, geometry, &liquify);
    }
    expect(
        automatic.dimensions == cpu.dimensions && automatic.bytes == cpu.bytes,
        "automatic Liquify replays the complete request through the CPU reference path"
    );
    expect(
        analyzed.execution.valid()
            && analyzed.execution.adjustment_backend == image::EditPreviewBackend::cpu
            && analyzed.execution.display_backend == image::EditPreviewBackend::cpu
            && analyzed.execution.adjustment_fell_back
            && analyzed.execution.display_fell_back
            && analyzed.execution.diagnostic.find("Liquify") != std::string::npos,
        "Liquify preview receipt names the atomic Metal-to-CPU fallback"
    );
}

} // namespace

int main() {
    public_preview_liquify_falls_back_atomically_to_the_cpu_sampler();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
