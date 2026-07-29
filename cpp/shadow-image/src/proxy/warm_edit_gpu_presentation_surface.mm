// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu_presentation_surface.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

namespace shadow::image::detail {

namespace {

inline constexpr std::size_t presentation_row_alignment = 256U;

inline constexpr std::string_view presentation_kernel_source = R"metal(
#include <metal_stdlib>
using namespace metal;

struct WarmPresentationParameters {
    uint width;
    uint height;
    uint source_row_bytes;
    uint target_row_bytes;
};

kernel void pack_warm_preview_rgba8_srgb_v1(
    device const uchar* source_rgb8 [[buffer(0)]],
    device uchar* target_rgba8 [[buffer(1)]],
    constant WarmPresentationParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint source_offset =
        position.y * parameters.source_row_bytes + position.x * 3u;
    const uint target_offset =
        position.y * parameters.target_row_bytes + position.x * 4u;
    target_rgba8[target_offset] = source_rgb8[source_offset];
    target_rgba8[target_offset + 1u] = source_rgb8[source_offset + 1u];
    target_rgba8[target_offset + 2u] = source_rgb8[source_offset + 2u];
    target_rgba8[target_offset + 3u] = 255u;
}
)metal";

struct WarmPresentationParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t source_row_bytes = 0U;
    std::uint32_t target_row_bytes = 0U;
};

static_assert(sizeof(WarmPresentationParameters) == 16U);

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

[[nodiscard]] bool checked_align(
    const std::uint64_t value,
    const std::uint64_t alignment,
    std::uint64_t& result
) noexcept {
    if (alignment == 0U || value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1U)) {
        return false;
    }
    result = ((value + alignment - 1U) / alignment) * alignment;
    return true;
}

[[nodiscard]] std::uint64_t next_surface_resource_id() noexcept {
    static std::atomic<std::uint64_t> next{1U};
    return next.fetch_add(1U, std::memory_order_relaxed);
}

class WarmPresentationPipeline final {
  public:
    WarmPresentationPipeline() = default;
    ~WarmPresentationPipeline() {
        [pipeline_ release];
        [device_ release];
    }

    WarmPresentationPipeline(const WarmPresentationPipeline&) = delete;
    WarmPresentationPipeline& operator=(const WarmPresentationPipeline&) = delete;

    [[nodiscard]] id<MTLComputePipelineState>
    pipeline(id<MTLDevice> device, std::string& diagnostic) {
        std::scoped_lock lock(mutex_);
        if (initialization_attempted_) {
            if (device_ != device) {
                diagnostic = "Metal presentation surface requested a different producer device";
                return nil;
            }
            diagnostic = initialization_diagnostic_;
            return pipeline_;
        }
        if (device == nil) {
            diagnostic = "Metal presentation surface has no producer device";
            return nil;
        }
        initialization_attempted_ = true;
        device_ = [device retain];
        ++compile_attempt_count_;
        const char* forced_failure =
            std::getenv("SHADOW_TEST_WARM_METAL_FORCE_PRESENTATION_PIPELINE_FAILURE");
        if (forced_failure != nullptr && std::string_view(forced_failure) == "1") {
            diagnostic = "test-injected Metal presentation surface pipeline failure";
            initialization_diagnostic_ = diagnostic;
            return nil;
        }

        NSString* source = [[NSString alloc] initWithBytes:presentation_kernel_source.data()
                                                    length:presentation_kernel_source.size()
                                                  encoding:NSUTF8StringEncoding];
        if (source == nil) {
            diagnostic = "Metal presentation surface shader is not valid UTF-8";
            initialization_diagnostic_ = diagnostic;
            return nil;
        }
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.mathMode = MTLMathModeSafe;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        [options release];
        [source release];
        if (library == nil) {
            diagnostic =
                "Metal presentation surface shader compilation failed: " + error_description(error);
            initialization_diagnostic_ = diagnostic;
            return nil;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"pack_warm_preview_rgba8_srgb_v1"];
        if (function == nil) {
            [library release];
            diagnostic = "Metal presentation surface shader entry point is unavailable";
            initialization_diagnostic_ = diagnostic;
            return nil;
        }
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                                                                                     error:&error];
        [function release];
        [library release];
        if (pipeline == nil) {
            diagnostic =
                "Metal presentation surface pipeline creation failed: " + error_description(error);
            initialization_diagnostic_ = diagnostic;
            return nil;
        }
        pipeline_ = pipeline;
        return pipeline_;
    }

    [[nodiscard]] std::uint64_t compile_attempt_count() const noexcept {
        std::scoped_lock lock(mutex_);
        return compile_attempt_count_;
    }

  private:
    mutable std::mutex mutex_;
    bool initialization_attempted_ = false;
    std::string initialization_diagnostic_;
    std::uint64_t compile_attempt_count_ = 0U;
    id<MTLDevice> device_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
};

[[nodiscard]] WarmPresentationPipeline& presentation_pipeline() {
    static WarmPresentationPipeline pipeline;
    return pipeline;
}

} // namespace

struct WarmEditGpuPresentationSurface::Storage final {
    Storage() = default;
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&&) = delete;
    Storage& operator=(Storage&&) = delete;

    Dimensions dimensions;
    std::uint32_t row_stride_bytes = 0U;
    WarmEditGpuPresentationPixelFormat pixel_format =
        WarmEditGpuPresentationPixelFormat::rgba8_unorm_display_srgb;
    std::uint64_t resource_id = 0U;
    id<MTLDevice> device = nil;
    id<MTLBuffer> buffer = nil;
    id<MTLTexture> texture = nil;

    ~Storage() {
        [texture release];
        [buffer release];
        [device release];
    }
};

WarmEditGpuPresentationSurface::WarmEditGpuPresentationSurface(std::unique_ptr<Storage> storage) :
    storage_(std::move(storage)) {}

WarmEditGpuPresentationSurface::~WarmEditGpuPresentationSurface() = default;

Dimensions WarmEditGpuPresentationSurface::dimensions() const noexcept {
    return storage_->dimensions;
}

std::uint32_t WarmEditGpuPresentationSurface::row_stride_bytes() const noexcept {
    return storage_->row_stride_bytes;
}

WarmEditGpuPresentationPixelFormat WarmEditGpuPresentationSurface::pixel_format() const noexcept {
    return storage_->pixel_format;
}

std::uint64_t WarmEditGpuPresentationSurface::resource_id() const noexcept {
    return storage_->resource_id;
}

std::uintptr_t WarmEditGpuPresentationSurface::native_texture_handle() const noexcept {
    return reinterpret_cast<std::uintptr_t>(storage_->texture);
}

std::uintptr_t WarmEditGpuPresentationSurface::native_device_handle() const noexcept {
    return reinterpret_cast<std::uintptr_t>(storage_->device);
}

std::span<const std::uint8_t> WarmEditGpuPresentationSurface::rgba8_rows() const noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>([storage_->buffer contents]);
    const auto byte_count =
        static_cast<std::size_t>(storage_->row_stride_bytes) * storage_->dimensions.height;
    return {bytes, byte_count};
}

std::vector<std::uint8_t> WarmEditGpuPresentationSurface::materialize_packed_rgb8() const {
    const auto width = storage_->dimensions.width;
    const auto height = storage_->dimensions.height;
    const std::uint64_t output_bytes = static_cast<std::uint64_t>(width) * height * 3U;
    if (output_bytes > std::numeric_limits<std::size_t>::max()) {
        return {};
    }
    std::vector<std::uint8_t> result(static_cast<std::size_t>(output_bytes));
    const auto rows = rgba8_rows();
    for (std::uint32_t row = 0U; row < height; ++row) {
        const auto* source =
            rows.data() + static_cast<std::size_t>(row) * storage_->row_stride_bytes;
        auto* target = result.data() + static_cast<std::size_t>(row) * width * 3U;
        for (std::uint32_t column = 0U; column < width; ++column) {
            std::memcpy(
                target + static_cast<std::size_t>(column) * 3U,
                source + static_cast<std::size_t>(column) * 4U,
                3U
            );
        }
    }
    return result;
}

WarmEditGpuPresentationSurfacePreparation
prepare_warm_edit_gpu_presentation_surface(id<MTLDevice> device, const Dimensions dimensions) {
    if (device == nil || dimensions.width == 0U || dimensions.height == 0U) {
        return {
            .surface = nullptr,
            .diagnostic = "Metal presentation surface has invalid dimensions or device",
        };
    }
    const std::string pipeline_diagnostic = prewarm_warm_edit_gpu_presentation_surface(device);
    if (!pipeline_diagnostic.empty()) {
        return {
            .surface = nullptr,
            .diagnostic = pipeline_diagnostic,
        };
    }
    std::uint64_t row_stride = 0U;
    const std::uint64_t tight_row = static_cast<std::uint64_t>(dimensions.width) * 4U;
    if (!checked_align(tight_row, presentation_row_alignment, row_stride)
        || row_stride > std::numeric_limits<std::uint32_t>::max()) {
        return {
            .surface = nullptr,
            .diagnostic = "Metal presentation surface row stride exceeds its descriptor",
        };
    }
    const std::uint64_t byte_count = row_stride * static_cast<std::uint64_t>(dimensions.height);
    if (byte_count == 0U || byte_count > std::numeric_limits<NSUInteger>::max()) {
        return {
            .surface = nullptr,
            .diagnostic = "Metal presentation surface exceeds the device address space",
        };
    }

    id<MTLBuffer> buffer = [device newBufferWithLength:static_cast<NSUInteger>(byte_count)
                                               options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        return {
            .surface = nullptr,
            .diagnostic = "Metal could not allocate an independent presentation buffer",
        };
    }
    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType2D;
    // The display kernel has already encoded these values for sRGB presentation.
    // Qt Quick's native texture wrapper does not accept a separate color-space
    // contract, so use a raw unorm view and preserve parity with the settled
    // QImage/JPEG path instead of applying the sRGB transfer a second time.
    descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.width = dimensions.width;
    descriptor.height = dimensions.height;
    descriptor.depth = 1U;
    descriptor.mipmapLevelCount = 1U;
    descriptor.sampleCount = 1U;
    descriptor.arrayLength = 1U;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.cpuCacheMode = MTLCPUCacheModeDefaultCache;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:descriptor
                                                       offset:0U
                                                  bytesPerRow:static_cast<NSUInteger>(row_stride)];
    [descriptor release];
    if (texture == nil) {
        [buffer release];
        return {
            .surface = nullptr,
            .diagnostic = "Metal could not create the buffer-backed sRGB presentation texture",
        };
    }

    // Populate the final owner directly. An aggregate temporary would shallow-copy Objective-C
    // handles and release them from its destructor before the returned surface can encode.
    auto storage = std::make_unique<WarmEditGpuPresentationSurface::Storage>();
    storage->dimensions = dimensions;
    storage->row_stride_bytes = static_cast<std::uint32_t>(row_stride);
    storage->pixel_format = WarmEditGpuPresentationPixelFormat::rgba8_unorm_display_srgb;
    storage->resource_id = next_surface_resource_id();
    storage->device = [device retain];
    storage->buffer = buffer;
    storage->texture = texture;
    return {
        .surface = std::shared_ptr<WarmEditGpuPresentationSurface>(
            new WarmEditGpuPresentationSurface(std::move(storage))
        ),
        .diagnostic = {},
    };
}

std::uint64_t warm_edit_gpu_presentation_pipeline_compile_attempt_count() noexcept {
    return presentation_pipeline().compile_attempt_count();
}

std::string prewarm_warm_edit_gpu_presentation_surface(id<MTLDevice> device) {
    std::string diagnostic;
    static_cast<void>(presentation_pipeline().pipeline(device, diagnostic));
    return diagnostic;
}

std::string encode_warm_edit_gpu_presentation_surface(
    id<MTLCommandBuffer> command_buffer,
    id<MTLBuffer> packed_rgb8,
    const WarmEditGpuPresentationSurface& surface
) {
    if (command_buffer == nil || packed_rgb8 == nil) {
        return "Metal presentation surface has no command or packed RGB source";
    }
    const auto dimensions = surface.storage_->dimensions;
    const std::uint64_t source_row = static_cast<std::uint64_t>(dimensions.width) * 3U;
    if (source_row > std::numeric_limits<std::uint32_t>::max()) {
        return "Metal presentation surface source row exceeds its kernel descriptor";
    }
    std::string diagnostic;
    id<MTLComputePipelineState> pipeline =
        presentation_pipeline().pipeline(surface.storage_->device, diagnostic);
    if (pipeline == nil) {
        return diagnostic.empty() ? "Metal presentation surface pipeline is unavailable"
                                  : diagnostic;
    }
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
        return "Metal could not create a presentation-surface encoder";
    }
    const WarmPresentationParameters parameters{
        .width = dimensions.width,
        .height = dimensions.height,
        .source_row_bytes = static_cast<std::uint32_t>(source_row),
        .target_row_bytes = surface.storage_->row_stride_bytes,
    };
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:packed_rgb8 offset:0U atIndex:0U];
    [encoder setBuffer:surface.storage_->buffer offset:0U atIndex:1U];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];

    const NSUInteger thread_width = std::max<NSUInteger>(1U, pipeline.threadExecutionWidth);
    const NSUInteger thread_height =
        std::max<NSUInteger>(1U, pipeline.maxTotalThreadsPerThreadgroup / thread_width);
    [encoder dispatchThreads:MTLSizeMake(dimensions.width, dimensions.height, 1U)
        threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
    [encoder endEncoding];
    return {};
}

} // namespace shadow::image::detail
