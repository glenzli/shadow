#include "metal_dcp_color_encoding.hpp"

#include "metal_raw_development.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace shadow::image::detail {

namespace {

[[nodiscard]] MetalDcpPostParameters make_parameters(const DcpColorTransform& transform) {
    // These are the CPU reference's fixed linear-sRGB <-> DCP ProPhoto working-space matrices,
    // evaluated from the same Bradford/D50/D65 constants.
    constexpr std::array<float, 9U> srgb_to_working{
        0.529392975772F, 0.330144038029F, 0.140570187253F,
        0.0983758859345F, 0.873417686110F, 0.0281631164354F,
        0.0168802149935F, 0.117659351989F, 0.865332752984F,
    };
    constexpr std::array<float, 9U> working_to_srgb{
        2.03414085953F, -0.727453293930F, -0.306687502239F,
        -0.228835551726F, 1.23175615200F, -0.00292063759050F,
        -0.00856559757293F, -0.153291432975F, 1.16185702602F,
    };
    MetalDcpPostParameters parameters{
        .srgb_to_working = srgb_to_working,
        .working_to_srgb = working_to_srgb,
    };
    const auto apply_table = [](
        const std::optional<DcpHsvTable>& table,
        std::uint32_t& hue_divisions,
        std::uint32_t& saturation_divisions,
        std::uint32_t& value_divisions,
        std::uint32_t& encoding_srgb
    ) {
        if (!table.has_value()) {
            return;
        }
        hue_divisions = table->hue_divisions;
        saturation_divisions = table->saturation_divisions;
        value_divisions = table->value_divisions;
        encoding_srgb = table->encoding == DcpTableEncoding::srgb ? 1U : 0U;
    };
    apply_table(
        transform.hue_sat_map,
        parameters.hue_hue_divisions,
        parameters.hue_saturation_divisions,
        parameters.hue_value_divisions,
        parameters.hue_encoding_srgb
    );
    apply_table(
        transform.look_table,
        parameters.look_hue_divisions,
        parameters.look_saturation_divisions,
        parameters.look_value_divisions,
        parameters.look_encoding_srgb
    );
    parameters.tone_curve_count = static_cast<std::uint32_t>(transform.tone_curve.size());
    return parameters;
}

[[nodiscard]] std::vector<MetalDcpHsvDelta>
pack_hsv_table(const std::optional<DcpHsvTable>& table) {
    std::vector<MetalDcpHsvDelta> packed;
    if (!table.has_value()) {
        return packed;
    }
    packed.reserve(table->entries.size());
    for (const DcpHsvDelta& entry : table->entries) {
        packed.push_back(MetalDcpHsvDelta{
            .hue_shift_degrees = entry.hue_shift_degrees,
            .saturation_scale = entry.saturation_scale,
            .value_scale = entry.value_scale,
        });
    }
    return packed;
}

[[nodiscard]] std::vector<MetalDcpToneCurvePoint>
pack_tone_curve(const DcpColorTransform& transform) {
    std::vector<MetalDcpToneCurvePoint> packed;
    packed.reserve(transform.tone_curve.size());
    for (std::size_t index = 0U; index < transform.tone_curve.size(); ++index) {
        packed.push_back(MetalDcpToneCurvePoint{
            .input = transform.tone_curve[index].input,
            .output = transform.tone_curve[index].output,
            .second_derivative =
                static_cast<float>(transform.tone_curve_second_derivatives[index]),
        });
    }
    return packed;
}

template <typename Element>
[[nodiscard]] id<MTLBuffer> make_table_buffer(
    const std::vector<Element>& values,
    const Element& empty_value
) {
    return [metal_raw_device()
        newBufferWithBytes:(values.empty() ? &empty_value : values.data())
        length:(values.empty() ? sizeof(Element) : values.size() * sizeof(Element))
        options:MTLResourceStorageModeShared];
}

} // namespace

MetalDcpColorEncoding::~MetalDcpColorEncoding() {
    [tone_buffer_ release];
    [look_buffer_ release];
    [hue_buffer_ release];
}

std::unique_ptr<MetalDcpColorEncoding> MetalDcpColorEncoding::prepare(
    const DcpColorTransform& transform,
    const std::uint32_t maximum_pixel_count,
    std::string& diagnostic
) {
    diagnostic.clear();
    if (!transform.valid() || !transform.has_post_matrix_stages() || maximum_pixel_count == 0U) {
        diagnostic = "Metal DCP encoding received an invalid transform or pixel bound";
        return nullptr;
    }
    if (!metal_dcp_color_development_available()) {
        diagnostic = metal_dcp_color_diagnostic();
        return nullptr;
    }

    const auto hue_table = pack_hsv_table(transform.hue_sat_map);
    const auto look_table = pack_hsv_table(transform.look_table);
    const auto tone_curve = pack_tone_curve(transform);
    std::size_t hue_bytes = 0U;
    std::size_t look_bytes = 0U;
    std::size_t tone_bytes = 0U;
    if (!checked_multiply(
            std::max<std::size_t>(hue_table.size(), 1U),
            sizeof(MetalDcpHsvDelta),
            hue_bytes
        )
        || !checked_multiply(
            std::max<std::size_t>(look_table.size(), 1U),
            sizeof(MetalDcpHsvDelta),
            look_bytes
        )
        || !checked_multiply(
            std::max<std::size_t>(tone_curve.size(), 1U),
            sizeof(MetalDcpToneCurvePoint),
            tone_bytes
        )) {
        diagnostic = "DCP table buffer size overflowed";
        return nullptr;
    }
    std::size_t resource_bytes = 0U;
    if (!checked_add(hue_bytes, look_bytes, resource_bytes)
        || !checked_add(resource_bytes, tone_bytes, resource_bytes)) {
        diagnostic = "DCP table working-set size overflowed";
        return nullptr;
    }

    auto result = std::unique_ptr<MetalDcpColorEncoding>(new MetalDcpColorEncoding);
    result->parameters_ = make_parameters(transform);
    result->maximum_pixel_count_ = maximum_pixel_count;
    result->resource_bytes_ = resource_bytes;
    const MetalDcpHsvDelta empty_delta{};
    const MetalDcpToneCurvePoint empty_curve{};
    result->hue_buffer_ = make_table_buffer(hue_table, empty_delta);
    result->look_buffer_ = make_table_buffer(look_table, empty_delta);
    result->tone_buffer_ = make_table_buffer(tone_curve, empty_curve);
    if (result->hue_buffer_ == nil || result->look_buffer_ == nil || result->tone_buffer_ == nil) {
        diagnostic = "Metal could not allocate one or more DCP table buffers";
        return nullptr;
    }
    return result;
}

std::size_t MetalDcpColorEncoding::resource_bytes() const noexcept {
    return resource_bytes_;
}

bool MetalDcpColorEncoding::encode(
    id<MTLComputeCommandEncoder> encoder,
    id<MTLBuffer> pixels,
    const std::uint32_t pixel_count,
    std::string& diagnostic
) const {
    diagnostic.clear();
    if (encoder == nil || pixels == nil || pixel_count == 0U
        || pixel_count > maximum_pixel_count_) {
        diagnostic = "Metal DCP encoding received an invalid pixel dispatch";
        return false;
    }
    MetalDcpPostParameters dispatch_parameters = parameters_;
    dispatch_parameters.pixel_count = pixel_count;
    const auto pipeline = metal_dcp_color_pipeline();
    const NSUInteger threads_per_group = std::min<NSUInteger>(
        256U,
        std::max<NSUInteger>(1U, pipeline.maxTotalThreadsPerThreadgroup)
    );
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:pixels offset:0U atIndex:0U];
    [encoder setBuffer:hue_buffer_ offset:0U atIndex:1U];
    [encoder setBuffer:look_buffer_ offset:0U atIndex:2U];
    [encoder setBuffer:tone_buffer_ offset:0U atIndex:3U];
    [encoder setBytes:&dispatch_parameters length:sizeof(dispatch_parameters) atIndex:4U];
    [encoder dispatchThreads:MTLSizeMake(pixel_count, 1U, 1U)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1U, 1U)];
    return true;
}

} // namespace shadow::image::detail
