// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "adjustment_execution_internal.hpp"

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

constexpr char metal_source[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

constant uint parameter_abi_version = 1u;
constant uint plan_identity_version = 1u;
constant uint opcode_white_balance = 1u;
constant uint opcode_exposure = 2u;
constant uint opcode_contrast = 3u;
constant uint opcode_saturation = 4u;
constant uint status_non_finite = 1u;
constant uint status_bad_abi = 2u;
constant uint status_bad_opcode = 4u;

struct MetalAdjustmentInvocationV1 {
    uint abi_version;
    uint plan_identity_version;
    uint width;
    uint height;
    uint input_row_floats;
    uint output_row_floats;
    uint step_count;
    uint reserved;
    float4 rgb_to_xyz_row_0;
    float4 rgb_to_xyz_row_1;
    float4 rgb_to_xyz_row_2;
    float4 xyz_to_rgb_row_0;
    float4 xyz_to_rgb_row_1;
    float4 xyz_to_rgb_row_2;
};

struct MetalAdjustmentOpV1 {
    uint opcode;
    uint source_node_index;
    uint reserved_0;
    uint reserved_1;
    float4 parameter_0;
    float4 parameter_1;
    float4 parameter_2;
};

struct MetalAdjustmentStatusV1 {
    atomic_uint flags;
    atomic_uint earliest_step;
    uint reserved_0;
    uint reserved_1;
};

inline float signed_cbrt(float value) {
    if (value == 0.0f) {
        return 0.0f;
    }
    // MSL has no cbrt overload. Safe-mode pow preserves signed extended-gamut inputs without
    // opting into the native/fast namespace.
    return copysign(pow(abs(value), 1.0f / 3.0f), value);
}

inline float3 multiply_rows(float4 row_0, float4 row_1, float4 row_2, float3 value) {
    return float3(
        dot(row_0.xyz, value),
        dot(row_1.xyz, value),
        dot(row_2.xyz, value)
    );
}

inline float3 xyz_to_oklab(float3 xyz) {
    const float l = signed_cbrt(
        0.8190224379967030f * xyz.x + 0.3619062600528904f * xyz.y
            - 0.1288737815209879f * xyz.z
    );
    const float m = signed_cbrt(
        0.0329836539323885f * xyz.x + 0.9292868615863434f * xyz.y
            + 0.0361446663506424f * xyz.z
    );
    const float s = signed_cbrt(
        0.0481771893596242f * xyz.x + 0.2642395317527308f * xyz.y
            + 0.6335478284694309f * xyz.z
    );
    return float3(
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s
    );
}

inline float3 oklab_to_xyz(float3 lab) {
    const float l_root = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
    const float m_root = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
    const float s_root = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
    const float l = l_root * l_root * l_root;
    const float m = m_root * m_root * m_root;
    const float s = s_root * s_root * s_root;
    return float3(
        1.2268798758459240f * l - 0.5578149944602170f * m
            + 0.2813910456659646f * s,
        -0.0405757452148009f * l + 1.1122868032803173f * m
            - 0.0717110580655164f * s,
        -0.0763729366746600f * l - 0.4214933324022431f * m
            + 1.5869240198367816f * s
    );
}

inline float3 working_rgb_to_oklab(
    float3 rgb,
    constant MetalAdjustmentInvocationV1& invocation
) {
    return xyz_to_oklab(multiply_rows(
        invocation.rgb_to_xyz_row_0,
        invocation.rgb_to_xyz_row_1,
        invocation.rgb_to_xyz_row_2,
        rgb
    ));
}

inline float3 oklab_to_working_rgb(
    float3 lab,
    constant MetalAdjustmentInvocationV1& invocation
) {
    return multiply_rows(
        invocation.xyz_to_rgb_row_0,
        invocation.xyz_to_rgb_row_1,
        invocation.xyz_to_rgb_row_2,
        oklab_to_xyz(lab)
    );
}

inline void report_failure(
    device MetalAdjustmentStatusV1& status,
    uint flag,
    uint step
) {
    atomic_fetch_or_explicit(&status.flags, flag, memory_order_relaxed);
    atomic_fetch_min_explicit(&status.earliest_step, step, memory_order_relaxed);
}

kernel void execute_adjustment_program_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    device const MetalAdjustmentOpV1* operations [[buffer(2)]],
    constant MetalAdjustmentInvocationV1& invocation [[buffer(3)]],
    device MetalAdjustmentStatusV1& status [[buffer(4)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= invocation.width || position.y >= invocation.height) {
        return;
    }
    if (invocation.abi_version != parameter_abi_version
        || invocation.plan_identity_version != plan_identity_version) {
        report_failure(status, status_bad_abi, 0u);
        return;
    }

    const uint input_index =
        position.y * invocation.input_row_floats + position.x * 3u;
    float3 rgb = float3(
        input[input_index],
        input[input_index + 1u],
        input[input_index + 2u]
    );

    for (uint step = 0u; step < invocation.step_count; ++step) {
        const MetalAdjustmentOpV1 operation = operations[step];
        switch (operation.opcode) {
        case opcode_white_balance:
            rgb = multiply_rows(
                operation.parameter_0,
                operation.parameter_1,
                operation.parameter_2,
                rgb
            );
            break;
        case opcode_exposure:
            rgb *= operation.parameter_0.x;
            break;
        case opcode_contrast: {
            float3 lab = working_rgb_to_oklab(rgb, invocation);
            if (lab.x > 0.0f && isfinite(lab.x)) {
                const float pivot = operation.parameter_0.x;
                if (operation.parameter_0.z != 0.0f) {
                    lab.x = pivot;
                } else {
                    const float normalized = lab.x / (lab.x + pivot);
                    const float amount = operation.parameter_0.y;
                    const float shaped = normalized
                        + amount * 2.0f * normalized * (1.0f - normalized)
                            * (2.0f * normalized - 1.0f);
                    const float bounded = clamp(shaped, 1.0e-7f, 1.0f - 1.0e-7f);
                    lab.x = pivot * bounded / (1.0f - bounded);
                }
                rgb = oklab_to_working_rgb(lab, invocation);
            }
            break;
        }
        case opcode_saturation:
            // Match the CPU oracle's exact neutral-axis bypass before any matrix round trip.
            if (!(rgb.x == rgb.y && rgb.y == rgb.z)) {
                float3 lab = working_rgb_to_oklab(rgb, invocation);
                lab.yz *= operation.parameter_0.x;
                rgb = oklab_to_working_rgb(lab, invocation);
            }
            break;
        default:
            report_failure(status, status_bad_opcode, step);
            return;
        }
        if (!all(isfinite(rgb))) {
            report_failure(status, status_non_finite, step);
            return;
        }
    }

    const uint output_index =
        position.y * invocation.output_row_floats + position.x * 3u;
    output[output_index] = rgb.x;
    output[output_index + 1u] = rgb.y;
    output[output_index + 2u] = rgb.z;
}
)METAL";

struct MetalAdjustmentStatusV1 final {
    std::uint32_t flags = 0U;
    std::uint32_t earliest_step = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t reserved_0 = 0U;
    std::uint32_t reserved_1 = 0U;
};

static_assert(sizeof(MetalAdjustmentStatusV1) == 16U);
static_assert(offsetof(MetalAdjustmentStatusV1, earliest_step) == 4U);

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
            NSString* source = [NSString stringWithUTF8String:metal_source];
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

MetalAdjustmentAttempt try_execute_adjustments_metal_v1(
    const FloatRgbImage& input,
    const PreparedMetalAdjustmentV1& program
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
        || program.operations.empty()) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal adjustment program failed its host ABI validation",
        };
    }

    std::size_t row_bytes = 0U;
    std::size_t operation_bytes = 0U;
    if (!checked_multiply(
            static_cast<std::size_t>(input.dimensions.width),
            3U * sizeof(float),
            row_bytes
        )
        || !checked_multiply(
            program.operations.size(),
            sizeof(MetalAdjustmentOpV1),
            operation_bytes
        )
        || row_bytes == 0U || operation_bytes == 0U) {
        return MetalAdjustmentAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal adjustment buffer size overflowed",
        };
    }

    const std::size_t maximum_buffer_bytes =
        static_cast<std::size_t>(context.device().maxBufferLength);
    if (row_bytes > maximum_buffer_bytes
        || operation_bytes > maximum_buffer_bytes
        || sizeof(MetalAdjustmentStatusV1) > maximum_buffer_bytes) {
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
                newBufferWithLength:sizeof(MetalAdjustmentStatusV1)
                options:MTLResourceStorageModeShared]
        );
        if (!input_buffer || !output_buffer || !operations_buffer || !status_buffer) {
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

            auto* status = static_cast<MetalAdjustmentStatusV1*>(
                [static_cast<id<MTLBuffer>>(status_buffer.get()) contents]
            );
            *status = MetalAdjustmentStatusV1{};
            MetalAdjustmentInvocationV1 invocation = program.invocation;
            invocation.height = current_rows;

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
