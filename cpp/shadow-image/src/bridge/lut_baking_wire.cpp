#include <shadow/image/cxx_bridge.hpp>
#include <shadow/image/lut_baking.hpp>

#include "adjustment_render_wire.hpp"

#include <string_view>

namespace shadow::bridge {

void validate_cube_lut_document(rust::Slice<const std::uint8_t> document) {
    static_cast<void>(image::parse_cube_lut(std::string_view{
        reinterpret_cast<const char*>(document.data()), document.size()
    }));
}

FfiBakedCubeLut bake_adjustment_cube_lut(
    const FfiAdjustmentRenderRequest& request,
    std::uint16_t size,
    const EditPreviewCancellationHandle& cancellation
) {
    auto layers = adjustment_render_wire::adjustment_layers(request.nodes);
    if (!layers) {
        layers = std::vector<image::AdjustmentLayer>{image::AdjustmentLayer{
            .layer_id = "lut-unmasked-stack",
            .nodes = adjustment_render_wire::adjustment_nodes(request.nodes),
        }};
    }
    const auto baked = image::bake_cube_lut(*layers, size, cancellation.token());
    const auto document = image::serialize_baked_cube_lut(baked);
    FfiBakedCubeLut result{
        .maximum_absolute_error = baked.maximum_absolute_error,
        .root_mean_square_error = baked.root_mean_square_error,
        .probe_count = baked.probe_count,
    };
    result.document.reserve(document.size());
    for (const char byte : document) {
        result.document.push_back(static_cast<std::uint8_t>(byte));
    }
    return result;
}

} // namespace shadow::bridge
