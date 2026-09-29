// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "metal_adjustment_execution.hpp"
#include "metal_adjustment_msl.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image::detail {

namespace {

constexpr std::string_view standalone_kernel_source = R"METAL(
kernel void execute_adjustment_program_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    device const MetalAdjustmentOp* operations [[buffer(2)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(3)]],
    device MetalAdjustmentStatus& status [[buffer(4)]],
    device const MetalCurveSegment* curve_segments [[buffer(5)]],
    device const float4* lut_entries [[buffer(6)]],
    device const float4* perceptual_mixer_entries [[buffer(7)]],
    device const MetalPerceptualRange* perceptual_range_entries [[buffer(8)]],
    device const float4* selective_color_entries [[buffer(9)]],
    device const float4* paint_entries [[buffer(10)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= invocation.width || position.y >= invocation.height) {
        return;
    }
    if (invocation.abi_version != parameter_abi_version
        || invocation.plan_identity_version != plan_identity_version) {
        report_adjustment_failure(status, status_bad_abi, 0u);
        return;
    }

    const uint input_index =
        position.y * invocation.input_row_floats + position.x * 3u;
    float3 rgb = float3(
        input[input_index],
        input[input_index + 1u],
        input[input_index + 2u]
    );

    if (!execute_adjustment_program(
            rgb,
            operations,
            curve_segments,
            lut_entries,
            perceptual_mixer_entries,
            perceptual_range_entries,
            selective_color_entries,
            paint_entries,
            position,
            invocation,
            status
        )) {
        return;
    }

    const uint output_index =
        position.y * invocation.output_row_floats + position.x * 3u;
    output[output_index] = rgb.x;
    output[output_index + 1u] = rgb.y;
    output[output_index + 2u] = rgb.z;
}
)METAL";

struct MetalAdjustmentStatus final {
    std::uint32_t flags = 0U;
    std::uint32_t earliest_step = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t reserved_0 = 0U;
    std::uint32_t reserved_1 = 0U;
};

static_assert(sizeof(MetalAdjustmentStatus) == 16U);
static_assert(offsetof(MetalAdjustmentStatus, earliest_step) == 4U);

class OwnedObjectiveCObject final {
public:
    explicit OwnedObjectiveCObject(id value = nil) noexcept
        : value_(value) {}

    ~OwnedObjectiveCObject() {
        [value_ release];
    }

    OwnedObjectiveCObject(const OwnedObjectiveCObject&) = delete;
    OwnedObjectiveCObject& operator=(const OwnedObjectiveCObject&) = delete;

    [[nodiscard]] id get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nil; }

private:
    id value_;
};

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

class MetalAdjustmentContext final {
public:
    MetalAdjustmentContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                diagnostic_ = "Metal could not create an adjustment command queue";
                return;
            }

            OwnedObjectiveCObject compile_options([[MTLCompileOptions alloc] init]);
            auto* options = static_cast<MTLCompileOptions*>(compile_options.get());
            options.mathMode = MTLMathModeSafe;
            NSError* error = nil;
            const std::string metal_source =
                make_metal_adjustment_source(standalone_kernel_source);
            OwnedObjectiveCObject source_object(
                [[NSString alloc] initWithBytes:metal_source.data()
                                        length:metal_source.size()
                                      encoding:NSUTF8StringEncoding]
            );
            if (!source_object) {
                diagnostic_ = "Metal adjustment shader source is not valid UTF-8";
                return;
            }
            auto* source = static_cast<NSString*>(source_object.get());
            OwnedObjectiveCObject library(
                [device_ newLibraryWithSource:source options:options error:&error]
            );
            if (!library) {
                diagnostic_ = "Metal adjustment shader compilation failed: "
                    + error_description(error);
                return;
            }
            OwnedObjectiveCObject function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"execute_adjustment_program_v1"]
            );
            if (!function) {
                diagnostic_ = "Metal adjustment shader entry point is unavailable";
                return;
            }
            pipeline_ = [device_ newComputePipelineStateWithFunction:
                static_cast<id<MTLFunction>>(function.get())
                error:&error];
            if (pipeline_ == nil) {
                diagnostic_ = "Metal adjustment pipeline creation failed: "
                    + error_description(error);
            }
        }
    }

    ~MetalAdjustmentContext() {
        [pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    MetalAdjustmentContext(const MetalAdjustmentContext&) = delete;
    MetalAdjustmentContext& operator=(const MetalAdjustmentContext&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return device_ != nil && queue_ != nil && pipeline_ != nil;
    }
    [[nodiscard]] id<MTLDevice> device() const noexcept { return device_; }
    [[nodiscard]] id<MTLCommandQueue> queue() const noexcept { return queue_; }
    [[nodiscard]] id<MTLComputePipelineState> pipeline() const noexcept {
        return pipeline_;
    }
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    std::string diagnostic_;
};

[[nodiscard]] MetalAdjustmentContext& metal_context() {
    // Source compilation and pipeline creation are process-local and paid once, keeping slider
    // interactions on the warm path.
    static MetalAdjustmentContext context;
    return context;
}

[[nodiscard]] std::mutex& metal_execution_mutex() {
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] bool checked_multiply(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result
) noexcept {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] std::size_t configured_tile_budget() noexcept {
    constexpr std::size_t desired = 128U * 1'024U * 1'024U;
    const char* configured = std::getenv("SHADOW_TEST_METAL_ADJUSTMENT_TILE_BYTES");
    if (configured == nullptr || *configured == '\0') {
        return desired;
    }
    std::size_t parsed = 0U;
    const std::string_view text(configured);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
        && parsed > 0U ? parsed : desired;
}

[[nodiscard]] bool force_test_failure() noexcept {
    const char* configured = std::getenv("SHADOW_TEST_METAL_ADJUSTMENT_FORCE_FAILURE");
    return configured != nullptr && std::string_view(configured) == "1";
}

[[nodiscard]] std::string command_buffer_diagnostic(
    id<MTLCommandBuffer> command_buffer
) {
    NSError* error = command_buffer.error;
    std::string detail = error_description(error);
    return detail.empty()
        ? "Metal adjustment command did not complete successfully"
        : "Metal adjustment command failed: " + detail;
}

} // namespace

bool metal_adjustment_available() noexcept {
    return metal_context().valid();
}

MetalAdjustmentAttempt try_execute_adjustments_metal(
    const FloatRgbImage& input,
    const PreparedMetalAdjustment& program
) {
    auto& context = metal_context();
    if (!context.valid()) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = context.diagnostic(),
        };
    }
    if (force_test_failure()) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = "test-injected Metal adjustment failure",
        };
    }
    if (program.invocation.abi_version != metal_adjustment_parameter_abi_version
        || program.invocation.plan_identity_version != edit_execution_plan_identity_version
        || program.invocation.width != input.dimensions.width
        || program.invocation.height != input.dimensions.height
        || program.invocation.step_count != program.operations.size()
        || program.invocation.curve_segment_count != program.curve_segments.size()
        || program.invocation.lut_entry_count != program.lut_entries.size()
        || program.invocation.perceptual_mixer_entry_count
            != program.perceptual_mixer_entries.size()
        || program.invocation.perceptual_range_entry_count
            != program.perceptual_range_entries.size()
        || program.invocation.selective_color_entry_count
            != program.selective_color_entries.size()
        || program.invocation.paint_entry_count != program.paint_entries.size()
        || program.operations.empty()) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal adjustment program failed its host ABI validation",
        };
    }

    std::size_t paint_bytes = 0U;
    std::size_t row_bytes = 0U;
    std::size_t operation_bytes = 0U;
    std::size_t curve_bytes = 0U;
    std::size_t lut_bytes = 0U;
    std::size_t perceptual_mixer_bytes = 0U;
    std::size_t perceptual_range_bytes = 0U;
    std::size_t selective_color_bytes = 0U;
    if (!checked_multiply(
            static_cast<std::size_t>(input.dimensions.width),
            3U * sizeof(float),
            row_bytes
        )
        || !checked_multiply(
            program.operations.size(),
            sizeof(MetalAdjustmentOp),
            operation_bytes
        )
        || !checked_multiply(
            program.curve_segments.size(),
            sizeof(MetalCurveSegment),
            curve_bytes
        )
        || !checked_multiply(
            program.lut_entries.size(),
            sizeof(MetalLutEntry),
            lut_bytes
        )
        || !checked_multiply(
            program.perceptual_mixer_entries.size(),
            sizeof(MetalPerceptualMixerEntry),
            perceptual_mixer_bytes
        )
        || !checked_multiply(
            program.perceptual_range_entries.size(),
            sizeof(MetalPerceptualRange),
            perceptual_range_bytes
        )
        || !checked_multiply(
            program.selective_color_entries.size(),
            sizeof(MetalSelectiveColorEntry),
            selective_color_bytes
        )
        || !checked_multiply(program.paint_entries.size(), sizeof(MetalPaintPixel), paint_bytes)
        || row_bytes == 0U || operation_bytes == 0U) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal adjustment buffer size overflowed",
        };
    }

    const std::size_t maximum_buffer_bytes =
        static_cast<std::size_t>(context.device().maxBufferLength);
    if (paint_bytes > maximum_buffer_bytes || row_bytes > maximum_buffer_bytes
        || operation_bytes > maximum_buffer_bytes
        || curve_bytes > maximum_buffer_bytes
        || lut_bytes > maximum_buffer_bytes
        || perceptual_mixer_bytes > maximum_buffer_bytes
        || perceptual_range_bytes > maximum_buffer_bytes
        || selective_color_bytes > maximum_buffer_bytes
        || sizeof(MetalAdjustmentStatus) > maximum_buffer_bytes) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal adjustment request exceeds this device's buffer limit",
        };
    }

    std::size_t tile_budget = configured_tile_budget();
    const auto recommended_working_set = static_cast<std::size_t>(
        context.device().recommendedMaxWorkingSetSize
    );
    if (recommended_working_set > 0U) {
        tile_budget = std::min(tile_budget, recommended_working_set / 3U);
    }
    const std::size_t combined_row_bytes = row_bytes * 2U;
    if (combined_row_bytes == 0U || combined_row_bytes > tile_budget) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = "one adjustment row exceeds Shadow's Metal working-set allowance",
        };
    }
    const std::size_t rows_by_budget = tile_budget / combined_row_bytes;
    const std::size_t rows_by_buffer = maximum_buffer_bytes / row_bytes;
    // Shader row/sample arithmetic is intentionally uint32. Cap each tile before casting so a
    // resource-rich future device cannot admit a buffer whose final active float index wraps.
    const std::size_t row_floats =
        static_cast<std::size_t>(program.invocation.input_row_floats);
    const std::size_t rows_by_shader_index =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
        / row_floats;
    const auto tile_rows = static_cast<std::uint32_t>(std::min({
        rows_by_budget,
        rows_by_buffer,
        rows_by_shader_index,
        static_cast<std::size_t>(input.dimensions.height),
    }));
    if (tile_rows == 0U) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal adjustment tile planner could not admit one row",
        };
    }

    std::size_t tile_bytes = 0U;
    if (!checked_multiply(row_bytes, tile_rows, tile_bytes)) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal adjustment tile size overflowed",
        };
    }

    std::lock_guard execution_lock(metal_execution_mutex());
    FloatRgbImage result = input;
    @autoreleasepool {
        OwnedObjectiveCObject input_buffer(
            [context.device()
                newBufferWithLength:tile_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject output_buffer(
            [context.device()
                newBufferWithLength:tile_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject operations_buffer(
            [context.device()
                newBufferWithBytes:program.operations.data()
                length:operation_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject status_buffer(
            [context.device()
                newBufferWithLength:sizeof(MetalAdjustmentStatus)
                options:MTLResourceStorageModeShared]
        );
        const MetalCurveSegment empty_curve{};
        const MetalLutEntry empty_lut{};
        const MetalPerceptualMixerEntry empty_perceptual_mixer{};
        const MetalPerceptualRange empty_perceptual_range{};
        const MetalSelectiveColorEntry empty_selective_color{};
        OwnedObjectiveCObject curve_buffer(
            [context.device()
                newBufferWithBytes:program.curve_segments.empty()
                    ? static_cast<const void*>(&empty_curve)
                    : static_cast<const void*>(program.curve_segments.data())
                length:program.curve_segments.empty()
                    ? sizeof(empty_curve)
                    : curve_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject lut_buffer(
            [context.device()
                newBufferWithBytes:program.lut_entries.empty()
                    ? static_cast<const void*>(&empty_lut)
                    : static_cast<const void*>(program.lut_entries.data())
                length:program.lut_entries.empty() ? sizeof(empty_lut) : lut_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject perceptual_mixer_buffer(
            [context.device()
                newBufferWithBytes:program.perceptual_mixer_entries.empty()
                    ? static_cast<const void*>(&empty_perceptual_mixer)
                    : static_cast<const void*>(
                        program.perceptual_mixer_entries.data()
                    )
                length:program.perceptual_mixer_entries.empty()
                    ? sizeof(empty_perceptual_mixer)
                    : perceptual_mixer_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject perceptual_range_buffer(
            [context.device()
                newBufferWithBytes:program.perceptual_range_entries.empty()
                    ? static_cast<const void*>(&empty_perceptual_range)
                    : static_cast<const void*>(
                        program.perceptual_range_entries.data()
                    )
                length:program.perceptual_range_entries.empty()
                    ? sizeof(empty_perceptual_range)
                    : perceptual_range_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject selective_color_buffer(
            [context.device()
                newBufferWithBytes:program.selective_color_entries.empty()
                    ? static_cast<const void*>(&empty_selective_color)
                    : static_cast<const void*>(
                        program.selective_color_entries.data()
                    )
                length:program.selective_color_entries.empty()
                    ? sizeof(empty_selective_color)
                    : selective_color_bytes
                options:MTLResourceStorageModeShared]
        );
        const MetalPaintPixel empty_paint{};
        OwnedObjectiveCObject paint_buffer([context.device()
            newBufferWithBytes:program.paint_entries.empty()
                                   ? static_cast<const void*>(&empty_paint)
                                   : static_cast<const void*>(program.paint_entries.data())
                        length:program.paint_entries.empty() ? sizeof(empty_paint) : paint_bytes
                       options:MTLResourceStorageModeShared]);
        if (!paint_buffer || !input_buffer || !output_buffer || !operations_buffer || !status_buffer
            || !curve_buffer || !lut_buffer || !perceptual_mixer_buffer
            || !perceptual_range_buffer || !selective_color_buffer) {
            return MetalAdjustmentAttempt{
                .output = std::nullopt,
                .diagnostic = "Metal could not allocate bounded adjustment buffers",
            };
        }

        const auto pipeline = context.pipeline();
        const NSUInteger thread_width = std::min<NSUInteger>(
            32U,
            std::max<NSUInteger>(1U, pipeline.threadExecutionWidth)
        );
        const NSUInteger thread_height = std::max<NSUInteger>(
            1U,
            std::min<NSUInteger>(
                8U,
                pipeline.maxTotalThreadsPerThreadgroup / thread_width
            )
        );
        const MTLSize threads_per_group =
            MTLSizeMake(thread_width, thread_height, 1U);
        const std::size_t source_stride_floats =
            input.row_stride_bytes / sizeof(float);
        const std::size_t result_stride_floats =
            result.row_stride_bytes / sizeof(float);

        for (std::uint32_t first_row = 0U;
             first_row < input.dimensions.height;
             first_row += tile_rows) {
            const std::uint32_t current_rows = std::min(
                tile_rows,
                input.dimensions.height - first_row
            );
            auto* input_destination = static_cast<float*>(
                [static_cast<id<MTLBuffer>>(input_buffer.get()) contents]
            );
            for (std::uint32_t local_y = 0U; local_y < current_rows; ++local_y) {
                const std::size_t source_offset =
                    static_cast<std::size_t>(first_row + local_y)
                    * source_stride_floats;
                std::memcpy(
                    input_destination
                        + static_cast<std::size_t>(local_y)
                            * program.invocation.input_row_floats,
                    input.samples.data() + source_offset,
                    row_bytes
                );
            }

            auto* status = static_cast<MetalAdjustmentStatus*>(
                [static_cast<id<MTLBuffer>>(status_buffer.get()) contents]
            );
            *status = MetalAdjustmentStatus{};
            MetalAdjustmentInvocation invocation = program.invocation;
            invocation.height = current_rows;
            invocation.paint_row_origin = first_row;

            id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
            if (command_buffer == nil || encoder == nil) {
                return MetalAdjustmentAttempt{
                    .output = std::nullopt,
                    .diagnostic = "Metal could not create an adjustment compute command",
                };
            }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(input_buffer.get())
                        offset:0U
                       atIndex:0U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(output_buffer.get())
                        offset:0U
                       atIndex:1U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(operations_buffer.get())
                        offset:0U
                       atIndex:2U];
            [encoder setBytes:&invocation
                       length:sizeof(invocation)
                      atIndex:3U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(status_buffer.get())
                        offset:0U
                       atIndex:4U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(curve_buffer.get())
                        offset:0U
                       atIndex:5U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(lut_buffer.get())
                        offset:0U
                       atIndex:6U];
            [encoder setBuffer:
                        static_cast<id<MTLBuffer>>(perceptual_mixer_buffer.get())
                        offset:0U
                       atIndex:7U];
            [encoder setBuffer:
                        static_cast<id<MTLBuffer>>(perceptual_range_buffer.get())
                        offset:0U
                       atIndex:8U];
            [encoder setBuffer:
                        static_cast<id<MTLBuffer>>(selective_color_buffer.get())
                        offset:0U
                       atIndex:9U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(paint_buffer.get())
                        offset:0U
                       atIndex:10U];
            [encoder dispatchThreads:MTLSizeMake(
                    input.dimensions.width,
                    current_rows,
                    1U
                )
                threadsPerThreadgroup:threads_per_group];
            [encoder endEncoding];
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                return MetalAdjustmentAttempt{
                    .output = std::nullopt,
                    .diagnostic = command_buffer_diagnostic(command_buffer),
                };
            }
            if (status->flags != 0U) {
                std::string diagnostic =
                    "Metal adjustment produced a non-finite or invalid result";
                if (status->earliest_step < program.operations.size()) {
                    diagnostic += " at source node "
                        + std::to_string(
                            program.operations[status->earliest_step].source_node_index
                        );
                }
                return MetalAdjustmentAttempt{
                    .output = std::nullopt,
                    .diagnostic = std::move(diagnostic),
                };
            }

            const auto* output_source = static_cast<const float*>(
                [static_cast<id<MTLBuffer>>(output_buffer.get()) contents]
            );
            for (std::uint32_t local_y = 0U; local_y < current_rows; ++local_y) {
                const std::size_t result_offset =
                    static_cast<std::size_t>(first_row + local_y)
                    * result_stride_floats;
                std::memcpy(
                    result.samples.data() + result_offset,
                    output_source
                        + static_cast<std::size_t>(local_y)
                            * program.invocation.output_row_floats,
                    row_bytes
                );
            }
        }
    }

    return MetalAdjustmentAttempt{
        .output = std::move(result),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
