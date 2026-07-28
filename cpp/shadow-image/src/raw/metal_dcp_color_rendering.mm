#include <shadow/image/dcp_color_development.hpp>

#include "metal_raw_development.hpp"
#include "metal_raw_runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

namespace shadow::image::detail {

namespace {

struct DcpHsvDeltaGpu final {
    float hue_shift_degrees = 0.0F;
    float saturation_scale = 1.0F;
    float value_scale = 1.0F;
};

struct DcpToneCurvePointGpu final {
    float input = 0.0F;
    float output = 0.0F;
    float second_derivative = 0.0F;
};

struct DcpPostParameters final {
    std::uint32_t pixel_count = 0U;
    std::uint32_t hue_hue_divisions = 0U;
    std::uint32_t hue_saturation_divisions = 0U;
    std::uint32_t hue_value_divisions = 0U;
    std::uint32_t hue_encoding_srgb = 0U;
    std::uint32_t look_hue_divisions = 0U;
    std::uint32_t look_saturation_divisions = 0U;
    std::uint32_t look_value_divisions = 0U;
    std::uint32_t look_encoding_srgb = 0U;
    std::uint32_t tone_curve_count = 0U;
    float srgb_to_working[9]{};
    float working_to_srgb[9]{};
};

static_assert(sizeof(DcpHsvDeltaGpu) == 12U);
static_assert(sizeof(DcpToneCurvePointGpu) == 12U);
static_assert(sizeof(DcpPostParameters) == 112U);
static_assert(offsetof(DcpPostParameters, pixel_count) == 0U);
static_assert(offsetof(DcpPostParameters, tone_curve_count) == 36U);
static_assert(offsetof(DcpPostParameters, srgb_to_working) == 40U);
static_assert(offsetof(DcpPostParameters, working_to_srgb) == 76U);


[[nodiscard]] DcpPostParameters make_dcp_post_parameters(
    const DcpColorTransform& transform
) {
    // These are the CPU reference's fixed linear-sRGB <-> DCP ProPhoto working-space matrices,
    // evaluated from the same Bradford/D50/D65 constants.  They intentionally live at this
    // executor boundary rather than in a Recipe node or a user-visible LUT.
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
    DcpPostParameters parameters;
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
    std::copy(
        srgb_to_working.begin(),
        srgb_to_working.end(),
        parameters.srgb_to_working
    );
    std::copy(
        working_to_srgb.begin(),
        working_to_srgb.end(),
        parameters.working_to_srgb
    );
    return parameters;
}

[[nodiscard]] std::vector<DcpHsvDeltaGpu> pack_dcp_hsv_table(
    const std::optional<DcpHsvTable>& table
) {
    std::vector<DcpHsvDeltaGpu> packed;
    if (!table.has_value()) {
        return packed;
    }
    packed.reserve(table->entries.size());
    for (const DcpHsvDelta& entry : table->entries) {
        packed.push_back(DcpHsvDeltaGpu{
            .hue_shift_degrees = entry.hue_shift_degrees,
            .saturation_scale = entry.saturation_scale,
            .value_scale = entry.value_scale,
        });
    }
    return packed;
}

[[nodiscard]] std::vector<DcpToneCurvePointGpu> pack_dcp_tone_curve(
    const DcpColorTransform& transform
) {
    std::vector<DcpToneCurvePointGpu> packed;
    packed.reserve(transform.tone_curve.size());
    for (std::size_t index = 0U; index < transform.tone_curve.size(); ++index) {
        packed.push_back(DcpToneCurvePointGpu{
            .input = transform.tone_curve[index].input,
            .output = transform.tone_curve[index].output,
            .second_derivative = static_cast<float>(transform.tone_curve_second_derivatives[index]),
        });
    }
    return packed;
}


} // namespace

MetalDcpColorDevelopmentAttempt try_apply_dcp_color_rendering_stages_metal(
    SceneLinearRgbFrame& pixels,
    const DcpColorTransform& transform
) {
    if (!pixels.valid() || !transform.valid() || !transform.has_post_matrix_stages()) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "Metal DCP executor received an invalid scene-linear frame or transform",
        };
    }
    if (pixels.samples.size() % 3U != 0U) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "Metal DCP executor requires packed RGB scene-linear samples",
        };
    }
    for (const float sample : pixels.samples) {
        if (!std::isfinite(sample)) {
            // Match the CPU contract, which rejects non-finite source values instead of silently
            // allowing a GPU kernel to leave one unprocessed pixel behind.
            return MetalDcpColorDevelopmentAttempt{
                .applied = false,
                .diagnostic = "DCP input rendering received non-finite scene-linear samples",
            };
        }
    }

    if (!metal_dcp_color_development_available()) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = metal_dcp_color_diagnostic(),
        };
    }

    std::size_t pixel_bytes = 0U;
    if (!checked_multiply(pixels.samples.size(), sizeof(float), pixel_bytes)
        || pixel_bytes == 0U
        || pixel_bytes > static_cast<std::size_t>(metal_raw_device().maxBufferLength)) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "scene-linear DCP buffer exceeds this Metal device's limit",
        };
    }
    const std::vector<DcpHsvDeltaGpu> hue_table = pack_dcp_hsv_table(transform.hue_sat_map);
    const std::vector<DcpHsvDeltaGpu> look_table = pack_dcp_hsv_table(transform.look_table);
    const std::vector<DcpToneCurvePointGpu> tone_curve = pack_dcp_tone_curve(transform);
    std::size_t hue_bytes = 0U;
    std::size_t look_bytes = 0U;
    std::size_t tone_bytes = 0U;
    if (!checked_multiply(hue_table.size(), sizeof(DcpHsvDeltaGpu), hue_bytes)
        || !checked_multiply(look_table.size(), sizeof(DcpHsvDeltaGpu), look_bytes)
        || !checked_multiply(tone_curve.size(), sizeof(DcpToneCurvePointGpu), tone_bytes)) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "DCP table buffer size overflowed",
        };
    }
    std::size_t working_set = 0U;
    if (!checked_add(pixel_bytes, hue_bytes, working_set)
        || !checked_add(working_set, look_bytes, working_set)
        || !checked_add(working_set, tone_bytes, working_set)) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "DCP Metal working-set size overflowed",
        };
    }
    const auto recommended_working_set = static_cast<std::size_t>(
        metal_raw_device().recommendedMaxWorkingSetSize
    );
    if (recommended_working_set > 0U && working_set > recommended_working_set / 3U) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "DCP scene-linear frame exceeds Shadow's Metal working-set allowance",
        };
    }

    const DcpPostParameters parameters = make_dcp_post_parameters(transform);
    if (parameters.pixel_count != 0U) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "internal DCP Metal parameter state was not initialized",
        };
    }
    if (pixels.samples.size() / 3U > std::numeric_limits<std::uint32_t>::max()) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "DCP scene-linear pixel count exceeds Metal's dispatch range",
        };
    }
    DcpPostParameters dispatch_parameters = parameters;
    dispatch_parameters.pixel_count = static_cast<std::uint32_t>(pixels.samples.size() / 3U);

    // Serialize the command queue and input copy with the other large RAW operations.  The DCP
    // tables are tiny, while a full-resolution fp32 RGB frame is not.
    std::lock_guard execution_lock(metal_execution_mutex());
    @autoreleasepool {
        const DcpHsvDeltaGpu empty_delta{};
        const DcpToneCurvePointGpu empty_curve{};
        OwnedObjectiveCObject pixel_buffer(
            [metal_raw_device()
                newBufferWithBytes:pixels.samples.data()
                length:pixel_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject hue_buffer(
            [metal_raw_device()
                newBufferWithBytes:(hue_table.empty() ? &empty_delta : hue_table.data())
                length:(hue_table.empty() ? sizeof(empty_delta) : hue_bytes)
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject look_buffer(
            [metal_raw_device()
                newBufferWithBytes:(look_table.empty() ? &empty_delta : look_table.data())
                length:(look_table.empty() ? sizeof(empty_delta) : look_bytes)
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject tone_buffer(
            [metal_raw_device()
                newBufferWithBytes:(tone_curve.empty() ? &empty_curve : tone_curve.data())
                length:(tone_curve.empty() ? sizeof(empty_curve) : tone_bytes)
                options:MTLResourceStorageModeShared]
        );
        if (!pixel_buffer || !hue_buffer || !look_buffer || !tone_buffer) {
            return MetalDcpColorDevelopmentAttempt{
                .applied = false,
                .diagnostic = "Metal could not allocate one or more DCP input buffers",
            };
        }
        const auto pipeline = metal_dcp_color_pipeline();
        const NSUInteger threads_per_group = std::min<NSUInteger>(
            256U,
            std::max<NSUInteger>(1U, pipeline.maxTotalThreadsPerThreadgroup)
        );
        id<MTLCommandBuffer> command_buffer = [metal_raw_command_queue() commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (command_buffer == nil || encoder == nil) {
            return MetalDcpColorDevelopmentAttempt{
                .applied = false,
                .diagnostic = "Metal could not create a DCP compute command",
            };
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(pixel_buffer.get()) offset:0U atIndex:0U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(hue_buffer.get()) offset:0U atIndex:1U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(look_buffer.get()) offset:0U atIndex:2U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(tone_buffer.get()) offset:0U atIndex:3U];
        [encoder setBytes:&dispatch_parameters
                   length:sizeof(dispatch_parameters)
                  atIndex:4U];
        [encoder dispatchThreads:MTLSizeMake(dispatch_parameters.pixel_count, 1U, 1U)
            threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1U, 1U)];
        [encoder endEncoding];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return MetalDcpColorDevelopmentAttempt{
                .applied = false,
                .diagnostic = metal_raw_command_buffer_diagnostic(command_buffer),
            };
        }
        std::memcpy(
            pixels.samples.data(),
            [static_cast<id<MTLBuffer>>(pixel_buffer.get()) contents],
            pixel_bytes
        );
    }
    return MetalDcpColorDevelopmentAttempt{
        .applied = true,
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
