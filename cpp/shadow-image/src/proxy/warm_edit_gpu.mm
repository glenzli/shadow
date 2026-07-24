// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image::detail {

namespace {

inline constexpr std::size_t warm_slot_count = 2U;
inline constexpr std::size_t maximum_warm_adjustment_operations = 256U;

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

struct WarmDisplayParameters {
    uint output_origin_x;
    uint output_origin_y;
    uint apply_scene_curve;
    uint retain_linear;
};

struct WarmStatus {
    atomic_uint flags;
    atomic_uint earliest_step;
    uint reserved_0;
    uint reserved_1;
};

inline float signed_cbrt(float value) {
    if (value == 0.0f) {
        return 0.0f;
    }
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

inline float3 linear_srgb_to_oklab(float3 rgb) {
    const float l = signed_cbrt(
        0.4122214708f * rgb.r + 0.5363325363f * rgb.g + 0.0514459929f * rgb.b
    );
    const float m = signed_cbrt(
        0.2119034982f * rgb.r + 0.6806995451f * rgb.g + 0.1073969566f * rgb.b
    );
    const float s = signed_cbrt(
        0.0883024619f * rgb.r + 0.2817188376f * rgb.g + 0.6299787005f * rgb.b
    );
    return float3(
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s
    );
}

inline float3 oklab_to_linear_srgb(float3 lab) {
    const float l_root = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
    const float m_root = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
    const float s_root = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
    const float l = l_root * l_root * l_root;
    const float m = m_root * m_root * m_root;
    const float s = s_root * s_root * s_root;
    return float3(
        4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s
    );
}

inline bool inside_display_srgb(float3 rgb) {
    return isfinite(rgb.r) && isfinite(rgb.g) && isfinite(rgb.b)
        && rgb.r >= 0.0f && rgb.r <= 1.0f
        && rgb.g >= 0.0f && rgb.g <= 1.0f
        && rgb.b >= 0.0f && rgb.b <= 1.0f;
}

inline float scene_luminance_to_display_luminance(float luminance) {
    if (!(luminance > 0.0f)) {
        return 0.0f;
    }
    constexpr float maximum_safe_luminance = 1.0e6f;
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    const float scene = min(luminance, maximum_safe_luminance);
    const float curved =
        scene * (a * scene + b) / (scene * (c * scene + d) + e);
    return clamp(curved / (a / c), 0.0f, 1.0f);
}

inline float3 neutral_scene_display_curve(float3 input) {
    const float luminance =
        input.r * 0.2126f + input.g * 0.7152f + input.b * 0.0722f;
    if (!(luminance > 0.0f)) {
        return input;
    }
    return input * (scene_luminance_to_display_luminance(luminance) / luminance);
}

inline float3 map_display_gamut(float3 input, bool apply_scene_curve) {
    const float3 display_linear = apply_scene_curve
        ? neutral_scene_display_curve(input)
        : input;
    if (inside_display_srgb(display_linear)) {
        return display_linear;
    }

    float3 lab = linear_srgb_to_oklab(display_linear);
    lab.x = clamp(lab.x, 0.0f, 1.0f);
    const float chroma = length(lab.yz);
    float3 best = oklab_to_linear_srgb(float3(lab.x, 0.0f, 0.0f));
    if (!isfinite(chroma) || chroma <= 1.0e-15f) {
        return clamp(best, 0.0f, 1.0f);
    }

    const float2 direction = lab.yz / chroma;
    float lower = 0.0f;
    float upper = min(chroma, 0.5f);
    for (uint iteration = 0u; iteration < 16u; ++iteration) {
        const float candidate_chroma = (lower + upper) * 0.5f;
        const float3 candidate = oklab_to_linear_srgb(float3(
            lab.x,
            direction.x * candidate_chroma,
            direction.y * candidate_chroma
        ));
        if (inside_display_srgb(candidate)) {
            lower = candidate_chroma;
            best = candidate;
        } else {
            upper = candidate_chroma;
        }
    }
    return clamp(best, 0.0f, 1.0f);
}

inline float display_dither(uint x, uint y) {
    uint state = x * 0x9e3779b9u ^ y * 0x85ebca6bu;
    state ^= state >> 16u;
    state *= 0x7feb352du;
    state ^= state >> 15u;
    state *= 0x846ca68bu;
    state ^= state >> 16u;
    const float unit = float(state) / float(0xffffffffu);
    return (unit - 0.5f) * 0.90f;
}

inline uchar encode_srgb8(float linear_sample, float dither) {
    const float linear = clamp(linear_sample, 0.0f, 1.0f);
    const float encoded = linear <= 0.0031308f
        ? 12.92f * linear
        : 1.055f * pow(linear, 1.0f / 2.4f) - 0.055f;
    return uchar(clamp(floor(encoded * 255.0f + dither + 0.5f), 0.0f, 255.0f));
}

inline void report_failure(
    device WarmStatus& status,
    uint flag,
    uint step
) {
    atomic_fetch_or_explicit(&status.flags, flag, memory_order_relaxed);
    atomic_fetch_min_explicit(&status.earliest_step, step, memory_order_relaxed);
}

kernel void render_warm_preview_v2(
    device const float* source [[buffer(0)]],
    device float* adjusted [[buffer(1)]],
    device uchar* display_rgb8 [[buffer(2)]],
    device const MetalAdjustmentOpV1* operations [[buffer(3)]],
    constant MetalAdjustmentInvocationV1& invocation [[buffer(4)]],
    constant WarmDisplayParameters& display [[buffer(5)]],
    device WarmStatus& status [[buffer(6)]],
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
        source[input_index],
        source[input_index + 1u],
        source[input_index + 2u]
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

    if (display.retain_linear != 0u) {
        const uint adjusted_index =
            position.y * invocation.output_row_floats + position.x * 3u;
        adjusted[adjusted_index] = rgb.x;
        adjusted[adjusted_index + 1u] = rgb.y;
        adjusted[adjusted_index + 2u] = rgb.z;
    }

    const float3 mapped = map_display_gamut(
        rgb,
        display.apply_scene_curve != 0u
    );
    const float dither = display_dither(
        display.output_origin_x + position.x,
        display.output_origin_y + position.y
    );
    const uint output_index =
        (position.y * invocation.width + position.x) * 3u;
    display_rgb8[output_index] = encode_srgb8(mapped.r, dither);
    display_rgb8[output_index + 1u] = encode_srgb8(mapped.g, dither);
    display_rgb8[output_index + 2u] = encode_srgb8(mapped.b, dither);
}
)METAL";

struct WarmDisplayParameters final {
    std::uint32_t output_origin_x = 0U;
    std::uint32_t output_origin_y = 0U;
    std::uint32_t apply_scene_curve = 0U;
    std::uint32_t retain_linear = 0U;
};

struct WarmStatus final {
    std::uint32_t flags = 0U;
    std::uint32_t earliest_step = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t reserved_0 = 0U;
    std::uint32_t reserved_1 = 0U;
};

static_assert(sizeof(WarmDisplayParameters) == 16U);
static_assert(sizeof(WarmStatus) == 16U);

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

[[nodiscard]] bool checked_add(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result
) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

class WarmMetalContext final {
public:
    WarmMetalContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                diagnostic_ = "Metal could not create a warm-preview command queue";
                return;
            }

            MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
            options.mathMode = MTLMathModeSafe;
            NSError* error = nil;
            NSString* source = [NSString stringWithUTF8String:metal_source];
            id<MTLLibrary> library =
                [device_ newLibraryWithSource:source options:options error:&error];
            [options release];
            if (library == nil) {
                diagnostic_ = "Metal warm-preview shader compilation failed: "
                    + error_description(error);
                return;
            }
            id<MTLFunction> function =
                [library newFunctionWithName:@"render_warm_preview_v2"];
            [library release];
            if (function == nil) {
                diagnostic_ = "Metal warm-preview shader entry point is unavailable";
                return;
            }
            pipeline_ = [device_ newComputePipelineStateWithFunction:function error:&error];
            [function release];
            if (pipeline_ == nil) {
                diagnostic_ = "Metal warm-preview pipeline creation failed: "
                    + error_description(error);
            }
        }
    }

    ~WarmMetalContext() {
        [pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    WarmMetalContext(const WarmMetalContext&) = delete;
    WarmMetalContext& operator=(const WarmMetalContext&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return device_ != nil && queue_ != nil && pipeline_ != nil;
    }
    [[nodiscard]] id<MTLDevice> device() const noexcept { return device_; }
    [[nodiscard]] id<MTLCommandQueue> queue() const noexcept { return queue_; }
    [[nodiscard]] id<MTLComputePipelineState> pipeline() const noexcept {
        return pipeline_;
    }
    [[nodiscard]] const std::string& diagnostic() const noexcept {
        return diagnostic_;
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    std::string diagnostic_;
};

[[nodiscard]] WarmMetalContext& metal_context() {
    // Device, queue and immutable pipeline state are process-wide. MTLCommandQueue is safe for
    // concurrent command-buffer creation; mutable raster resources remain session/slot-local.
    static WarmMetalContext context;
    return context;
}

[[nodiscard]] bool force_test_failure() noexcept {
    const char* configured = std::getenv("SHADOW_TEST_WARM_METAL_FORCE_FAILURE");
    return configured != nullptr && std::string_view(configured) == "1";
}

[[nodiscard]] std::string command_buffer_diagnostic(
    id<MTLCommandBuffer> command_buffer
) {
    const std::string detail = error_description(command_buffer.error);
    return detail.empty()
        ? "Metal warm-preview command did not complete successfully"
        : "Metal warm-preview command failed: " + detail;
}

struct WarmSlot final {
    id<MTLBuffer> adjusted = nil;
    id<MTLBuffer> rgb8 = nil;
    id<MTLBuffer> operations = nil;
    id<MTLBuffer> status = nil;
    bool busy = false;
};

} // namespace

struct WarmEditGpuSession::Impl final {
    id<MTLBuffer> source = nil;
    std::array<WarmSlot, warm_slot_count> slots;
    Dimensions dimensions;
    std::size_t row_stride_bytes = 0U;
    std::size_t sample_count = 0U;
    std::size_t adjusted_row_stride_bytes = 0U;
    std::size_t adjusted_sample_count = 0U;
    std::size_t adjusted_bytes = 0U;
    FloatPixelFormat pixel_format = FloatPixelFormat::unknown;
    TransferFunction transfer_function = TransferFunction::unknown;
    ImageReference reference = ImageReference::unknown;
    WorkingRgbSpace working_space;
    double level_zero_to_raster_scale_x = 1.0;
    double level_zero_to_raster_scale_y = 1.0;
    std::size_t rgb8_bytes = 0U;
    std::size_t operation_buffer_bytes = 0U;

    mutable std::mutex mutex;
    mutable std::condition_variable available_slot;
    mutable std::size_t next_slot = 0U;
    mutable std::uint64_t active_renders = 0U;
    mutable WarmEditPreviewGpuStats stats;

    ~Impl() {
        for (auto& slot : slots) {
            [slot.status release];
            [slot.operations release];
            [slot.rgb8 release];
            [slot.adjusted release];
        }
        [source release];
    }

    [[nodiscard]] std::size_t acquire_slot() {
        std::unique_lock lock(mutex);
        available_slot.wait(lock, [this]() {
            return std::ranges::any_of(slots, [](const WarmSlot& slot) {
                return !slot.busy;
            });
        });
        for (std::size_t offset = 0U; offset < slots.size(); ++offset) {
            const std::size_t index = (next_slot + offset) % slots.size();
            if (!slots[index].busy) {
                slots[index].busy = true;
                next_slot = (index + 1U) % slots.size();
                ++active_renders;
                ++stats.render_count;
                stats.peak_concurrent_renders =
                    std::max(stats.peak_concurrent_renders, active_renders);
                return index;
            }
        }
        std::abort();
    }

    void release_slot(const std::size_t index, const bool completed) noexcept {
        {
            std::lock_guard lock(mutex);
            slots[index].busy = false;
            --active_renders;
            if (completed) {
                ++stats.completed_render_count;
            }
        }
        available_slot.notify_one();
    }
};

WarmEditGpuSession::WarmEditGpuSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

WarmEditGpuSession::~WarmEditGpuSession() = default;

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const bool retain_linear_for_analysis
) const {
    if (!impl_) {
        return RenderAttempt{
            .output = std::nullopt,
            .diagnostic = "session-resident Metal warm preview is not initialized",
        };
    }
    if (force_test_failure()) {
        return RenderAttempt{
            .output = std::nullopt,
            .diagnostic = "test-injected session-resident Metal warm-preview failure",
        };
    }

    PreparedMetalAdjustmentV1 program;
    if (plan.segments.empty()) {
        program.invocation.width = impl_->dimensions.width;
        program.invocation.height = impl_->dimensions.height;
        program.invocation.input_row_floats =
            static_cast<std::uint32_t>(impl_->row_stride_bytes / sizeof(float));
        program.invocation.output_row_floats = static_cast<std::uint32_t>(
            impl_->adjusted_row_stride_bytes / sizeof(float)
        );
        program.invocation.step_count = 0U;
    } else {
        FloatRgbImage source_layout{
            .dimensions = impl_->dimensions,
            .row_stride_bytes = impl_->row_stride_bytes,
            .pixel_format = impl_->pixel_format,
            .transfer_function = impl_->transfer_function,
            .reference = impl_->reference,
            .working_space = impl_->working_space,
            .level_zero_to_raster_scale_x = impl_->level_zero_to_raster_scale_x,
            .level_zero_to_raster_scale_y = impl_->level_zero_to_raster_scale_y,
            // The immutable source was fully validated before upload. Warm parameter
            // preparation uses only layout/color metadata and therefore intentionally skips a
            // repeated full-raster finiteness scan.
            .samples = {},
        };
        auto preparation = prepare_metal_adjustment_v1(
            source_layout,
            nodes,
            plan,
            AdjustmentExecutionContext{.full_dimensions = impl_->dimensions},
            true
        );
        if (!preparation.program.has_value()) {
            return RenderAttempt{
                .output = std::nullopt,
                .diagnostic = preparation.diagnostic.empty()
                    ? "session-resident Metal warm preview could not prepare the adjustment plan"
                    : std::move(preparation.diagnostic),
            };
        }
        program = std::move(*preparation.program);
        program.invocation.input_row_floats =
            static_cast<std::uint32_t>(impl_->row_stride_bytes / sizeof(float));
        program.invocation.output_row_floats = static_cast<std::uint32_t>(
            impl_->adjusted_row_stride_bytes / sizeof(float)
        );
    }
    if (program.operations.size() > maximum_warm_adjustment_operations) {
        return RenderAttempt{
            .output = std::nullopt,
            .diagnostic =
                "session-resident Metal warm preview exceeds its 256-operation slot capacity",
        };
    }

    const std::size_t slot_index = impl_->acquire_slot();
    bool completed = false;
    struct SlotRelease final {
        Impl& impl;
        std::size_t index;
        bool& completed;
        ~SlotRelease() { impl.release_slot(index, completed); }
    } release{*impl_, slot_index, completed};
    WarmSlot& slot = impl_->slots[slot_index];

    @autoreleasepool {
        const std::size_t operation_bytes =
            program.operations.size() * sizeof(MetalAdjustmentOpV1);
        if (operation_bytes > 0U) {
            std::memcpy(
                [slot.operations contents],
                program.operations.data(),
                operation_bytes
            );
        }
        auto* status = static_cast<WarmStatus*>([slot.status contents]);
        *status = WarmStatus{};
        const WarmDisplayParameters display{
            .apply_scene_curve =
                impl_->reference == ImageReference::scene_referred ? 1U : 0U,
            .retain_linear = retain_linear_for_analysis ? 1U : 0U,
        };

        auto& context = metal_context();
        id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (command_buffer == nil || encoder == nil) {
            return RenderAttempt{
                .output = std::nullopt,
                .diagnostic = "Metal could not create a warm-preview compute command",
            };
        }
        [encoder setComputePipelineState:context.pipeline()];
        [encoder setBuffer:impl_->source offset:0U atIndex:0U];
        [encoder setBuffer:slot.adjusted offset:0U atIndex:1U];
        [encoder setBuffer:slot.rgb8 offset:0U atIndex:2U];
        [encoder setBuffer:slot.operations offset:0U atIndex:3U];
        [encoder setBytes:&program.invocation
                   length:sizeof(program.invocation)
                  atIndex:4U];
        [encoder setBytes:&display length:sizeof(display) atIndex:5U];
        [encoder setBuffer:slot.status offset:0U atIndex:6U];

        const NSUInteger thread_width = std::min<NSUInteger>(
            32U,
            std::max<NSUInteger>(1U, context.pipeline().threadExecutionWidth)
        );
        const NSUInteger thread_height = std::max<NSUInteger>(
            1U,
            std::min<NSUInteger>(
                8U,
                context.pipeline().maxTotalThreadsPerThreadgroup / thread_width
            )
        );
        [encoder dispatchThreads:MTLSizeMake(
                impl_->dimensions.width,
                impl_->dimensions.height,
                1U
            )
            threadsPerThreadgroup:MTLSizeMake(
                thread_width,
                thread_height,
                1U
            )];
        [encoder endEncoding];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return RenderAttempt{
                .output = std::nullopt,
                .diagnostic = command_buffer_diagnostic(command_buffer),
            };
        }
        if (status->flags != 0U) {
            std::string diagnostic =
                "session-resident Metal warm preview produced an invalid result";
            if (status->earliest_step < program.operations.size()) {
                diagnostic += " at source node "
                    + std::to_string(
                        program.operations[status->earliest_step].source_node_index
                    );
            }
            return RenderAttempt{
                .output = std::nullopt,
                .diagnostic = std::move(diagnostic),
            };
        }

        RenderResult result{
            .dimensions = impl_->dimensions,
            .rgb8 = std::vector<std::uint8_t>(impl_->rgb8_bytes),
            .analyzed_linear = std::nullopt,
            .had_active_adjustments = !plan.segments.empty(),
        };
        std::memcpy(result.rgb8.data(), [slot.rgb8 contents], impl_->rgb8_bytes);
        if (retain_linear_for_analysis) {
            FloatRgbImage linear{
                .dimensions = impl_->dimensions,
                .row_stride_bytes = impl_->adjusted_row_stride_bytes,
                .pixel_format = impl_->pixel_format,
                .transfer_function = impl_->transfer_function,
                .reference = impl_->reference,
                .working_space = impl_->working_space,
                .level_zero_to_raster_scale_x = impl_->level_zero_to_raster_scale_x,
                .level_zero_to_raster_scale_y = impl_->level_zero_to_raster_scale_y,
                .samples = std::vector<float>(impl_->adjusted_sample_count),
            };
            std::memcpy(
                linear.samples.data(),
                [slot.adjusted contents],
                impl_->adjusted_bytes
            );
            result.analyzed_linear = std::move(linear);
        }
        completed = true;
        return RenderAttempt{
            .output = std::move(result),
            .diagnostic = {},
        };
    }
}

WarmEditPreviewGpuStats WarmEditGpuSession::stats() const noexcept {
    if (!impl_) {
        return {};
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

WarmEditGpuPreparation prepare_warm_edit_gpu_session(const FloatRgbImage& source) {
    auto& context = metal_context();
    if (!context.valid()) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = context.diagnostic(),
        };
    }
    if (source.dimensions.width == 0U || source.dimensions.height == 0U
        || source.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || source.transfer_function != TransferFunction::linear
        || (source.reference != ImageReference::scene_referred
            && source.reference != ImageReference::display_referred)
        || source.row_stride_bytes % sizeof(float) != 0U
        || source.row_stride_bytes / sizeof(float)
            < static_cast<std::size_t>(source.dimensions.width) * 3U
        || source.row_stride_bytes / sizeof(float)
            > std::numeric_limits<std::uint32_t>::max()
        || source.dimensions.width > std::numeric_limits<std::uint32_t>::max() / 3U) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview source does not satisfy the resident Metal layout",
        };
    }
    const std::size_t row_floats = source.row_stride_bytes / sizeof(float);
    std::size_t sample_count = 0U;
    if (!checked_multiply(
            row_floats,
            static_cast<std::size_t>(source.dimensions.height),
            sample_count
        )
        || source.samples.size() != sample_count) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview source storage does not match its declared layout",
        };
    }
    for (const float sample : source.samples) {
        if (!std::isfinite(sample)) {
            return WarmEditGpuPreparation{
                .session = nullptr,
                .diagnostic = "warm-preview source contains a non-finite sample",
            };
        }
    }

    std::size_t source_bytes = 0U;
    std::size_t adjusted_sample_count = 0U;
    std::size_t adjusted_bytes = 0U;
    std::size_t rgb8_bytes = 0U;
    if (!checked_multiply(sample_count, sizeof(float), source_bytes)
        || !checked_multiply(
            static_cast<std::size_t>(source.dimensions.pixel_count()),
            3U,
            adjusted_sample_count
        )
        || !checked_multiply(
            adjusted_sample_count,
            sizeof(float),
            adjusted_bytes
        )
        || !checked_multiply(
            static_cast<std::size_t>(source.dimensions.pixel_count()),
            3U,
            rgb8_bytes
        )
        || source_bytes == 0U || adjusted_bytes == 0U || rgb8_bytes == 0U) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview resident Metal buffer size overflowed",
        };
    }
    constexpr std::size_t operation_buffer_bytes =
        maximum_warm_adjustment_operations * sizeof(MetalAdjustmentOpV1);
    const std::size_t maximum_buffer_bytes =
        static_cast<std::size_t>(context.device().maxBufferLength);
    if (source_bytes > maximum_buffer_bytes
        || adjusted_bytes > maximum_buffer_bytes
        || rgb8_bytes > maximum_buffer_bytes
        || operation_buffer_bytes > maximum_buffer_bytes) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview resident buffers exceed this Metal device's limit",
        };
    }

    std::size_t per_slot_bytes = 0U;
    std::size_t slots_bytes = 0U;
    std::size_t resident_bytes = 0U;
    if (!checked_add(adjusted_bytes, rgb8_bytes, per_slot_bytes)
        || !checked_add(per_slot_bytes, operation_buffer_bytes, per_slot_bytes)
        || !checked_add(per_slot_bytes, sizeof(WarmStatus), per_slot_bytes)
        || !checked_multiply(per_slot_bytes, warm_slot_count, slots_bytes)
        || !checked_add(source_bytes, slots_bytes, resident_bytes)) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview resident working-set size overflowed",
        };
    }
    const std::uint64_t recommended = context.device().recommendedMaxWorkingSetSize;
    if (recommended > 0U
        && resident_bytes > static_cast<std::size_t>(recommended / 2U)) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic =
                "warm-preview resident buffers exceed half the recommended Metal working set",
        };
    }

    auto impl = std::make_unique<WarmEditGpuSession::Impl>();
    impl->dimensions = source.dimensions;
    impl->row_stride_bytes = source.row_stride_bytes;
    impl->sample_count = sample_count;
    impl->adjusted_row_stride_bytes =
        static_cast<std::size_t>(source.dimensions.width) * 3U * sizeof(float);
    impl->adjusted_sample_count = adjusted_sample_count;
    impl->adjusted_bytes = adjusted_bytes;
    impl->pixel_format = source.pixel_format;
    impl->transfer_function = source.transfer_function;
    impl->reference = source.reference;
    impl->working_space = source.working_space;
    impl->level_zero_to_raster_scale_x = source.level_zero_to_raster_scale_x;
    impl->level_zero_to_raster_scale_y = source.level_zero_to_raster_scale_y;
    impl->rgb8_bytes = rgb8_bytes;
    impl->operation_buffer_bytes = operation_buffer_bytes;
    impl->stats = WarmEditPreviewGpuStats{
        .resident = true,
        .source_upload_count = 1U,
        .gpu_buffer_allocation_count = 1U + warm_slot_count * 4U,
        .resident_bytes = resident_bytes,
    };

    @autoreleasepool {
        impl->source = [context.device()
            newBufferWithBytes:source.samples.data()
            length:source_bytes
            options:MTLResourceStorageModeShared];
        if (impl->source == nil) {
            return WarmEditGpuPreparation{
                .session = nullptr,
                .diagnostic = "Metal could not upload the immutable warm-preview source",
            };
        }
        for (auto& slot : impl->slots) {
            slot.adjusted = [context.device()
                newBufferWithLength:adjusted_bytes
                options:MTLResourceStorageModeShared];
            slot.rgb8 = [context.device()
                newBufferWithLength:rgb8_bytes
                options:MTLResourceStorageModeShared];
            slot.operations = [context.device()
                newBufferWithLength:operation_buffer_bytes
                options:MTLResourceStorageModeShared];
            slot.status = [context.device()
                newBufferWithLength:sizeof(WarmStatus)
                options:MTLResourceStorageModeShared];
            if (slot.adjusted == nil || slot.rgb8 == nil
                || slot.operations == nil || slot.status == nil) {
                return WarmEditGpuPreparation{
                    .session = nullptr,
                    .diagnostic =
                        "Metal could not allocate both warm-preview execution slots",
                };
            }
        }
    }
    return WarmEditGpuPreparation{
        .session = std::shared_ptr<WarmEditGpuSession>(
            new WarmEditGpuSession(std::move(impl))
        ),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
