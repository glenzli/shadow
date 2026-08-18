// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "full_edit_detail_metal_source.hpp"

#include "full_edit_detail_metal_source_msl.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::image::detail {

class MetalSceneLinearRegionDeviceAccess final {
  public:
    [[nodiscard]] static id<MTLBuffer> buffer(const MetalSceneLinearRegionLease& lease) noexcept {
        return (id<MTLBuffer>)lease.native_buffer_handle();
    }

    [[nodiscard]] static id<MTLDevice> device(const MetalSceneLinearRegionLease& lease) noexcept {
        return (id<MTLDevice>)lease.native_device_handle();
    }

    [[nodiscard]] static id<MTLCommandQueue>
    queue(const MetalSceneLinearRegionLease& lease) noexcept {
        return (id<MTLCommandQueue>)lease.native_queue_handle();
    }
};

namespace {

inline constexpr float source_curve_hdr_handoff_width = 0.25F;
inline constexpr double maximum_source_curve_terminal_slope = 2.0;

class OwnedObjectiveCObject final {
  public:
    explicit OwnedObjectiveCObject(id value = nil) noexcept : value_(value) {}
    ~OwnedObjectiveCObject() {
        [value_ release];
    }

    OwnedObjectiveCObject(const OwnedObjectiveCObject&) = delete;
    OwnedObjectiveCObject& operator=(const OwnedObjectiveCObject&) = delete;
    OwnedObjectiveCObject(OwnedObjectiveCObject&& other) noexcept :
        value_(std::exchange(other.value_, nil)) {}
    OwnedObjectiveCObject& operator=(OwnedObjectiveCObject&& other) noexcept {
        if (this != &other) {
            [value_ release];
            value_ = std::exchange(other.value_, nil);
        }
        return *this;
    }

    [[nodiscard]] id get() const noexcept {
        return value_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nil;
    }

  private:
    id value_ = nil;
};

struct SourceCurveSegment final {
    float x0 = 0.0F;
    float x1 = 0.0F;
    float y0 = 0.0F;
    float y1 = 0.0F;
    float m0 = 0.0F;
    float m1 = 0.0F;
};

struct SourceRenderingParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t row_floats = 0U;
    std::uint32_t segment_count = 0U;
    float exposure_gain = 1.0F;
    float hdr_handoff_width = source_curve_hdr_handoff_width;
    float hdr_terminal_output = 1.0F;
    float hdr_terminal_slope = 1.0F;
};

static_assert(sizeof(SourceCurveSegment) == 24U);
static_assert(sizeof(SourceRenderingParameters) == 32U);

[[nodiscard]] bool environment_enabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    NSString* description = [error localizedDescription];
    const char* text = [description UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

class FullEditDetailMetalSourceContext final {
  public:
    FullEditDetailMetalSourceContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
            if (@available(macOS 15.0, *)) {
                options.mathMode = MTLMathModeSafe;
            } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                options.fastMathEnabled = NO;
#pragma clang diagnostic pop
            }
            NSError* error = nil;
            const std::string source_text(full_edit_detail_metal_source_msl);
            NSString* source = [NSString stringWithUTF8String:source_text.c_str()];
            OwnedObjectiveCObject library([device_ newLibraryWithSource:source
                                                                options:options
                                                                  error:&error]);
            [options release];
            if (!library) {
                diagnostic_ = "full-detail Metal source-rendering shader compilation failed: "
                              + error_description(error);
                return;
            }
            OwnedObjectiveCObject function([static_cast<id<MTLLibrary>>(library.get())
                newFunctionWithName:@"apply_full_edit_source_rendering"]);
            if (!function) {
                diagnostic_ = "full-detail Metal source-rendering entry point is unavailable";
                return;
            }
            pipeline_ = [device_
                newComputePipelineStateWithFunction:static_cast<id<MTLFunction>>(function.get())
                                              error:&error];
            if (pipeline_ == nil) {
                diagnostic_ = "full-detail Metal source-rendering pipeline creation failed: "
                              + error_description(error);
            }
        }
    }

    ~FullEditDetailMetalSourceContext() {
        [pipeline_ release];
        [device_ release];
    }

    FullEditDetailMetalSourceContext(const FullEditDetailMetalSourceContext&) = delete;
    FullEditDetailMetalSourceContext& operator=(const FullEditDetailMetalSourceContext&) = delete;

    [[nodiscard]] bool available() const noexcept {
        return device_ != nil && pipeline_ != nil;
    }

    id<MTLDevice> device_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    std::string diagnostic_;
};

[[nodiscard]] FullEditDetailMetalSourceContext& context() {
    static FullEditDetailMetalSourceContext instance;
    return instance;
}

[[nodiscard]] double
tangent_at(const std::vector<SourceToneCurvePoint>& curve, const std::size_t index) noexcept {
    if (index == 0U) {
        return (curve[1U].output - curve[0U].output) / (curve[1U].input - curve[0U].input);
    }
    if (index + 1U == curve.size()) {
        return (curve[index].output - curve[index - 1U].output)
               / (curve[index].input - curve[index - 1U].input);
    }
    const double left_interval = curve[index].input - curve[index - 1U].input;
    const double right_interval = curve[index + 1U].input - curve[index].input;
    const double left_slope = (curve[index].output - curve[index - 1U].output) / left_interval;
    const double right_slope = (curve[index + 1U].output - curve[index].output) / right_interval;
    if (left_slope <= 0.0 || right_slope <= 0.0) {
        return 0.0;
    }
    const double weight_left = 2.0 * right_interval + left_interval;
    const double weight_right = right_interval + 2.0 * left_interval;
    return (weight_left + weight_right) / (weight_left / left_slope + weight_right / right_slope);
}

[[nodiscard]] std::vector<SourceCurveSegment>
prepare_curve_segments(const SourceRenderingReceipt& receipt) {
    // This validates schema, kind, identity, finite exposure, curve ordering, and placeholders
    // through the canonical public owner before the device transaction is admitted.
    static_cast<void>(source_rendering_identity(receipt));
    const auto& curve = receipt.luminance_tone_curve;
    std::vector<SourceCurveSegment> segments;
    if (curve.empty()) {
        return segments;
    }
    segments.reserve(curve.size() - 1U);
    for (std::size_t index = 0U; index + 1U < curve.size(); ++index) {
        segments.push_back(
            SourceCurveSegment{
                .x0 = static_cast<float>(curve[index].input),
                .x1 = static_cast<float>(curve[index + 1U].input),
                .y0 = static_cast<float>(curve[index].output),
                .y1 = static_cast<float>(curve[index + 1U].output),
                .m0 = static_cast<float>(tangent_at(curve, index)),
                .m1 = static_cast<float>(tangent_at(curve, index + 1U)),
            }
        );
    }
    return segments;
}

[[nodiscard]] double terminal_slope(const std::vector<SourceToneCurvePoint>& curve) noexcept {
    if (curve.size() < 2U) {
        return 1.0;
    }
    const std::size_t last = curve.size() - 1U;
    const double input_span = curve[last].input - curve[last - 1U].input;
    if (!(input_span > 0.0)) {
        return 0.0;
    }
    const double slope = (curve[last].output - curve[last - 1U].output) / input_span;
    return std::isfinite(slope) ? std::clamp(slope, 0.0, maximum_source_curve_terminal_slope) : 0.0;
}

[[nodiscard]] std::uint64_t
checked_rgb_bytes(const GeometryPixelRect rect, const char* diagnostic) {
    const std::uint64_t pixels = static_cast<std::uint64_t>(rect.width) * rect.height;
    if (rect.width == 0U || rect.height == 0U
        || pixels > std::numeric_limits<std::uint64_t>::max() / (3U * sizeof(float))) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, diagnostic);
    }
    return pixels * 3U * sizeof(float);
}

[[nodiscard]] std::uint64_t resident_allowance(id<MTLDevice> device) noexcept {
    const std::uint64_t recommended = device.recommendedMaxWorkingSetSize;
    return recommended == 0U
               ? maximum_full_edit_detail_metal_resident_bytes
               : std::min(maximum_full_edit_detail_metal_resident_bytes, recommended / 2U);
}

[[nodiscard]] bool
checked_add(const std::uint64_t left, const std::uint64_t right, std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] WorkingRgbSpace linear_srgb_working_space() {
    return WorkingRgbSpace{
        .id = "srgb-d65-linear",
        .primaries =
            {
                Chromaticity{0.6400, 0.3300},
                Chromaticity{0.3000, 0.6000},
                Chromaticity{0.1500, 0.0600},
            },
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] std::string command_diagnostic(id<MTLCommandBuffer> command_buffer) {
    NSString* description = [command_buffer.error localizedDescription];
    const char* text = [description UTF8String];
    return text == nullptr || *text == '\0'
               ? "full-detail Metal source rendering did not complete successfully"
               : std::string(text);
}

} // namespace

MetalSourceRenderingInPlaceAttempt apply_source_rendering_in_place_metal(
    void* const native_device_handle,
    void* const native_queue_handle,
    void* const native_buffer_handle,
    const Dimensions dimensions,
    const SourceRenderingReceipt& source_rendering
) {
    MetalSourceRenderingInPlaceAttempt result;
    try {
        auto& runtime = context();
        const id<MTLDevice> device = static_cast<id<MTLDevice>>(native_device_handle);
        const id<MTLCommandQueue> queue = static_cast<id<MTLCommandQueue>>(native_queue_handle);
        const id<MTLBuffer> buffer = static_cast<id<MTLBuffer>>(native_buffer_handle);
        if (!runtime.available()) {
            result.diagnostic = runtime.diagnostic_.empty()
                                    ? "Metal source rendering is unavailable"
                                    : runtime.diagnostic_;
            return result;
        }
        if (device == nil || queue == nil || buffer == nil
            || static_cast<std::uint64_t>(device.registryID)
                   != static_cast<std::uint64_t>(runtime.device_.registryID)) {
            result.diagnostic = "Metal source rendering cannot adopt this device resource";
            return result;
        }
        const std::uint64_t output_bytes = checked_rgb_bytes(
            GeometryPixelRect{0U, 0U, dimensions.width, dimensions.height},
            "Metal source-rendering output size overflows"
        );
        if (static_cast<std::uint64_t>(buffer.length) < output_bytes) {
            result.diagnostic = "Metal source-rendering buffer is smaller than its RGB layout";
            return result;
        }
        std::vector<SourceCurveSegment> curve = prepare_curve_segments(source_rendering);
        if (curve.size() > std::numeric_limits<std::uint32_t>::max()) {
            result.diagnostic = "Metal source-rendering curve is too large";
            return result;
        }
        constexpr SourceCurveSegment empty_segment{};
        const std::size_t curve_bytes =
            curve.empty() ? sizeof(empty_segment) : curve.size() * sizeof(SourceCurveSegment);
        OwnedObjectiveCObject curve_buffer([device
            newBufferWithBytes:curve.empty() ? static_cast<const void*>(&empty_segment)
                                             : static_cast<const void*>(curve.data())
                        length:curve_bytes
                       options:MTLResourceStorageModeShared]);
        if (!curve_buffer) {
            result.diagnostic = "Metal source-rendering curve allocation failed";
            return result;
        }
        const double exposure = std::exp2(source_rendering.total_exposure_stops());
        if (!std::isfinite(exposure) || exposure > std::numeric_limits<float>::max()) {
            result.diagnostic = "Metal source-rendering exposure is invalid";
            return result;
        }
        SourceRenderingParameters parameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .row_floats = dimensions.width * 3U,
            .segment_count = static_cast<std::uint32_t>(curve.size()),
            .exposure_gain = static_cast<float>(exposure),
            .hdr_handoff_width = source_curve_hdr_handoff_width,
            .hdr_terminal_output =
                source_rendering.luminance_tone_curve.empty()
                    ? 1.0F
                    : static_cast<float>(source_rendering.luminance_tone_curve.back().output),
            .hdr_terminal_slope =
                static_cast<float>(terminal_slope(source_rendering.luminance_tone_curve)),
        };
        if (environment_enabled("SHADOW_TEST_FULL_EDIT_DETAIL_METAL_SOURCE_FAIL")) {
            result.diagnostic = "test-forced full-detail Metal source-rendering failure";
            return result;
        }
        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            command_buffer == nil ? nil : [command_buffer computeCommandEncoder];
        if (encoder == nil) {
            result.diagnostic = "Metal source rendering could not create a command";
            return result;
        }
        const NSUInteger thread_width = std::min<NSUInteger>(
            32U,
            std::max<NSUInteger>(1U, runtime.pipeline_.threadExecutionWidth)
        );
        const NSUInteger thread_height = std::max<NSUInteger>(
            1U,
            std::min<NSUInteger>(8U, runtime.pipeline_.maxTotalThreadsPerThreadgroup / thread_width)
        );
        [encoder setComputePipelineState:runtime.pipeline_];
        [encoder setBuffer:buffer offset:0U atIndex:0U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(curve_buffer.get()) offset:0U atIndex:1U];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
        [encoder dispatchThreads:MTLSizeMake(dimensions.width, dimensions.height, 1U)
            threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
        [encoder endEncoding];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            result.diagnostic = command_diagnostic(command_buffer);
            return result;
        }
        result.applied = true;
        result.curve_upload_bytes = curve.empty() ? 0U : static_cast<std::uint64_t>(curve_bytes);
        return result;
    } catch (const std::exception& error) {
        result.diagnostic = error.what();
        return result;
    }
}

FullEditDetailMetalSourcePreparation prepare_full_edit_detail_metal_source(
    raw_pipeline_detail::ResidentRawSource& source,
    const SourceRenderingReceipt& source_rendering,
    const GeometryPixelRect working_rect,
    const std::uint64_t other_cached_resident_bytes
) {
    FullEditDetailMetalSourcePreparation result;
    const auto fail = [&source, &result](std::string diagnostic) {
        source.invalidate_device_path();
        result.session.reset();
        result.diagnostic = std::move(diagnostic);
        return std::move(result);
    };
    if (!source.device_path_valid()) {
        return fail("full-detail Metal RAW source is unavailable or terminally invalidated");
    }

    try {
        auto& runtime = context();
        if (!runtime.available()) {
            return fail(
                runtime.diagnostic_.empty() ? "full-detail Metal source rendering is unavailable"
                                            : runtime.diagnostic_
            );
        }
        const auto& raw_source = source.metal_source();
        auto region = source.region_optics().prepare_region(working_rect);
        std::vector<SourceCurveSegment> curve = prepare_curve_segments(source_rendering);
        if (curve.size() > std::numeric_limits<std::uint32_t>::max()) {
            return fail("full-detail Metal source-rendering curve is too large");
        }
        constexpr SourceCurveSegment empty_segment{};
        const std::size_t curve_bytes =
            curve.empty() ? sizeof(empty_segment) : curve.size() * sizeof(SourceCurveSegment);
        const std::uint64_t output_bytes = checked_rgb_bytes(
            working_rect,
            "full-detail Metal source-rendering output size overflows"
        );
        const std::uint64_t coordinate_bytes =
            static_cast<std::uint64_t>(region.absolute_source_coordinates().size()) * sizeof(float);
        const std::uint64_t gain_bytes =
            static_cast<std::uint64_t>(region.profile_vignetting_gains().size()) * sizeof(float);

        if (!raw_source.telemetry().published) {
            return fail("full-detail Metal RAW source has no published device");
        }
        const std::uint64_t allowance = resident_allowance(runtime.device_);
        std::uint64_t non_raw_resident_bytes = 0U;
        for (const std::uint64_t addition : {
                 other_cached_resident_bytes,
                 output_bytes,
                 coordinate_bytes,
                 gain_bytes,
                 static_cast<std::uint64_t>(curve_bytes),
             }) {
            if (!checked_add(non_raw_resident_bytes, addition, non_raw_resident_bytes)) {
                return fail("full-detail Metal combined resident-byte estimate overflows");
            }
        }
        if (allowance == 0U || non_raw_resident_bytes >= allowance) {
            return fail("full-detail Metal RAW/optics transaction exceeds the combined budget");
        }
        const std::uint64_t raw_resident_allowance = allowance - non_raw_resident_bytes;
        if (raw_source.retained_bytes() > raw_resident_allowance) {
            return fail("full-detail Metal RAW source leaves no budget for its requested region");
        }
        MetalSceneLinearRegionLease optics = develop_metal_scene_linear_region_optics(
            source,
            std::move(region),
            raw_resident_allowance
        );
        if (raw_source.retained_bytes() > raw_resident_allowance) {
            return fail("full-detail Metal RAW source exceeded its admitted resident allowance");
        }
        result.telemetry.optics = optics.telemetry();

        id<MTLDevice> device = MetalSceneLinearRegionDeviceAccess::device(optics);
        id<MTLBuffer> buffer = MetalSceneLinearRegionDeviceAccess::buffer(optics);
        id<MTLCommandQueue> queue = MetalSceneLinearRegionDeviceAccess::queue(optics);
        if (device == nil || buffer == nil || queue == nil
            || static_cast<std::uint64_t>(device.registryID)
                   != static_cast<std::uint64_t>(runtime.device_.registryID)
            || static_cast<std::uint64_t>(buffer.length) < output_bytes) {
            return fail("full-detail Metal optics result cannot be adopted on this device");
        }

        const auto source_rendering_attempt = apply_source_rendering_in_place_metal(
            reinterpret_cast<void*>(device),
            reinterpret_cast<void*>(queue),
            reinterpret_cast<void*>(buffer),
            optics.dimensions(),
            source_rendering
        );
        if (!source_rendering_attempt.applied) {
            return fail(
                source_rendering_attempt.diagnostic.empty()
                    ? "full-detail Metal source rendering failed"
                    : source_rendering_attempt.diagnostic
            );
        }
        result.telemetry.raw = raw_source.telemetry();
        result.telemetry.source_rendering_dispatch_count = 1U;
        result.telemetry.source_rendering_curve_upload_count = curve.empty() ? 0U : 1U;
        result.telemetry.source_rendering_curve_upload_bytes =
            source_rendering_attempt.curve_upload_bytes;
        result.telemetry.resident_allowance_bytes = allowance;

        const std::uint64_t raw_resident_bytes = raw_source.retained_bytes();
        std::uint64_t external_resident_bytes = 0U;
        if (!checked_add(
                raw_resident_bytes,
                other_cached_resident_bytes,
                external_resident_bytes
            )) {
            return fail("full-detail Metal external resident-byte count overflows");
        }
        std::uint64_t preparation_external_resident_bytes = 0U;
        if (!checked_add(
                external_resident_bytes,
                static_cast<std::uint64_t>(curve_bytes),
                preparation_external_resident_bytes
            )) {
            return fail("full-detail Metal transient resident-byte count overflows");
        }
        auto warm = prepare_warm_edit_gpu_session(
            WarmEditGpuAdoptedSource{
                .dimensions = optics.dimensions(),
                .row_stride_bytes =
                    static_cast<std::size_t>(optics.dimensions().width) * 3U * sizeof(float),
                .native_device_handle = reinterpret_cast<void*>(device),
                .native_buffer_handle = reinterpret_cast<void*>(buffer),
                .source_buffer_bytes = output_bytes,
                .external_resident_bytes = preparation_external_resident_bytes,
                .resident_allowance_bytes = allowance,
                .working_space = linear_srgb_working_space(),
                .level_zero_to_raster_scale_x = 1.0,
                .level_zero_to_raster_scale_y = 1.0,
            }
        );
        if (!warm.session) {
            return fail(
                warm.diagnostic.empty() ? "full-detail Metal warm source adoption failed"
                                        : std::move(warm.diagnostic)
            );
        }

        result.session = std::move(warm.session);
        result.telemetry.warm = result.session->stats();
        result.telemetry.warm_source_adoption_count = 1U;
        if (!checked_add(
                external_resident_bytes,
                result.telemetry.warm.resident_bytes,
                result.telemetry.combined_resident_bytes
            )
            || result.telemetry.combined_resident_bytes > allowance
            || result.telemetry.warm.source_upload_count != 0U) {
            return fail("full-detail Metal source adoption violated its combined budget");
        }
        return result;
    } catch (const DecodeError& error) {
        return fail(error.what());
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}

bool full_edit_detail_metal_source_available() noexcept {
    return context().available() && metal_scene_linear_region_optics_available()
           && raw_pipeline_detail::metal_resident_raw_source_available();
}

const std::string& full_edit_detail_metal_source_diagnostic() noexcept {
    return context().diagnostic_;
}

} // namespace shadow::image::detail
