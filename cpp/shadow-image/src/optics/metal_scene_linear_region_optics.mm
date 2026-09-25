// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "metal_scene_linear_region_optics.hpp"

#include "manual_optics.hpp"
#include "metal_scene_linear_region_optics_msl.hpp"
#include "scene_linear_region_optics.hpp"

#include "../raw/metal_raw_development.hpp"
#include "../raw/metal_resident_raw_source.hpp"
#include "../raw/resident_raw_source.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image::detail {

class MetalSceneLinearRegionOpticsAccess final {
  public:
    [[nodiscard]] static id<MTLBuffer>
    buffer(const raw_pipeline_detail::MetalResidentRawRegionLease& lease) noexcept {
        return (id<MTLBuffer>)lease.native_buffer_handle();
    }

    [[nodiscard]] static id<MTLDevice>
    device(const raw_pipeline_detail::MetalResidentRawRegionLease& lease) noexcept {
        return (id<MTLDevice>)lease.native_device_handle();
    }

    [[nodiscard]] static GeometryPixelRect
    requested_core(const raw_pipeline_detail::MetalResidentRawRegionLease& lease) noexcept {
        return lease.requested_core();
    }

    [[nodiscard]] static Dimensions
    full_dimensions(const raw_pipeline_detail::MetalResidentRawRegionLease& lease) noexcept {
        return lease.full_dimensions();
    }

    static void invalidate(const raw_pipeline_detail::MetalResidentRawRegionLease& lease) noexcept {
        lease.invalidate_source();
    }
};

namespace {

struct MetalSceneLinearRegionOpticsParameters final {
    std::uint32_t full_width = 0U;
    std::uint32_t full_height = 0U;
    std::uint32_t source_origin_x = 0U;
    std::uint32_t source_origin_y = 0U;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    // Source RGB may be a compact region or a complete resident preview; gains always remain
    // compact and aligned with the exact logical source preimage above.
    std::uint32_t source_buffer_origin_x = 0U;
    std::uint32_t source_buffer_origin_y = 0U;
    std::uint32_t source_buffer_width = 0U;
    std::uint32_t output_origin_x = 0U;
    std::uint32_t output_origin_y = 0U;
    std::uint32_t output_width = 0U;
    std::uint32_t output_height = 0U;
    std::uint32_t has_source = 0U;
    std::uint32_t coordinate_remap = 0U;
    std::uint32_t profile_vignetting = 0U;
    float manual_vignette_amount = 0.0F;
    float manual_vignette_midpoint = 0.5F;
};

static_assert(sizeof(MetalSceneLinearRegionOpticsParameters) == 72U);

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
    id value_;
};

[[nodiscard]] bool environment_enabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

[[nodiscard]] bool rect_inside(const GeometryPixelRect rect, const Dimensions dimensions) noexcept {
    return rect.width != 0U && rect.height != 0U && rect.x < dimensions.width
           && rect.y < dimensions.height && rect.width <= dimensions.width - rect.x
           && rect.height <= dimensions.height - rect.y;
}

[[nodiscard]] std::size_t
checked_rgb_bytes(const std::uint32_t width, const std::uint32_t height, const char* diagnostic) {
    const std::size_t width_size = static_cast<std::size_t>(width);
    const std::size_t height_size = static_cast<std::size_t>(height);
    if (width == 0U || height == 0U || width_size > std::numeric_limits<std::size_t>::max() / 3U
        || height_size > std::numeric_limits<std::size_t>::max() / (width_size * 3U)
        || width_size * height_size * 3U
               > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, diagnostic);
    }
    return width_size * height_size * 3U * sizeof(float);
}

[[nodiscard]] std::size_t checked_float_bytes(const std::size_t count, const char* diagnostic) {
    if (count == 0U || count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(DecodeErrorCode::resource_limit, 0, diagnostic);
    }
    return count * sizeof(float);
}

class MetalSceneLinearRegionOpticsContext final {
  public:
    MetalSceneLinearRegionOpticsContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                diagnostic_ = "Metal scene-linear region optics could not create a command queue";
                return;
            }
            OwnedObjectiveCObject options_object([[MTLCompileOptions alloc] init]);
            auto* options = static_cast<MTLCompileOptions*>(options_object.get());
            options.mathMode = MTLMathModeSafe;
            NSError* error = nil;
            const std::string source_text(metal_scene_linear_region_optics_msl);
            NSString* source = [NSString stringWithUTF8String:source_text.c_str()];
            OwnedObjectiveCObject library([device_ newLibraryWithSource:source
                                                                options:options
                                                                  error:&error]);
            if (!library) {
                diagnostic_ = "Metal scene-linear region optics shader compilation failed: "
                              + error_description(error);
                return;
            }
            OwnedObjectiveCObject function([static_cast<id<MTLLibrary>>(library.get())
                newFunctionWithName:@"apply_scene_linear_region_optics"]);
            if (!function) {
                diagnostic_ = "Metal scene-linear region optics shader entry point is unavailable";
                return;
            }
            pipeline_ = [device_
                newComputePipelineStateWithFunction:static_cast<id<MTLFunction>>(function.get())
                                              error:&error];
            if (pipeline_ == nil) {
                diagnostic_ = "Metal scene-linear region optics pipeline creation failed: "
                              + error_description(error);
            }
        }
    }

    ~MetalSceneLinearRegionOpticsContext() {
        [pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    MetalSceneLinearRegionOpticsContext(const MetalSceneLinearRegionOpticsContext&) = delete;
    MetalSceneLinearRegionOpticsContext&
    operator=(const MetalSceneLinearRegionOpticsContext&) = delete;

    [[nodiscard]] bool available() const noexcept {
        return device_ != nil && queue_ != nil && pipeline_ != nil;
    }

    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    std::string diagnostic_;
    std::atomic<std::uint64_t> next_fence_value_{1U};
};

[[nodiscard]] MetalSceneLinearRegionOpticsContext& context() {
    static MetalSceneLinearRegionOpticsContext instance;
    return instance;
}

void validate_prepared_region(
    const std::optional<raw_pipeline_detail::MetalResidentRawRegionLease>& source_preimage,
    const PreparedSceneLinearRegionOptics& optics,
    const lensfun_modifier_plan::PreparedRegion& region
) {
    const Dimensions full_dimensions = optics.full_dimensions();
    const GeometryPixelRect output_rect = region.output_rect();
    const auto& region_source_preimage = region.source_preimage();
    const auto& absolute_source_coordinates = region.absolute_source_coordinates();
    const auto& profile_vignetting_gains = region.profile_vignetting_gains();
    manual_optics::validate_settings(region.manual_output_settings());
    if (!optics.owns_region(region)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal region optics received evidence from a different prepared plan"
        );
    }
    if (manual_optics::has_manual_geometry(region.manual_output_settings())) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Metal region optics accepts profile geometry followed only by manual output "
            "vignetting"
        );
    }
    if (full_dimensions.width == 0U || full_dimensions.height == 0U
        || !rect_inside(output_rect, full_dimensions)
        || region.executor_identity() != lensfun_modifier_plan::region_executor_identity) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal region optics received invalid C-a region evidence"
        );
    }

    const std::size_t output_pixels = checked_rgb_bytes(
                                          output_rect.width,
                                          output_rect.height,
                                          "Metal region optics output dimensions overflow"
                                      )
                                      / (3U * sizeof(float));
    const std::size_t expected_coordinates = output_pixels * 6U;
    if (region.coordinate_remap()) {
        if (absolute_source_coordinates.size() != expected_coordinates) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Metal region optics coordinate table does not match its output rectangle"
            );
        }
    } else if (!absolute_source_coordinates.empty()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal pointwise region optics unexpectedly carries remap coordinates"
        );
    }

    if (!region_source_preimage.has_value()) {
        if (!region.coordinate_remap() || source_preimage.has_value()
            || !profile_vignetting_gains.empty()) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Metal all-out-of-bounds optics evidence has inconsistent source state"
            );
        }
        return;
    }
    if (!source_preimage.has_value()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal region optics is missing its exact RAW device preimage"
        );
    }
    const GeometryPixelRect source_rect = *region_source_preimage;
    if (!rect_inside(source_rect, full_dimensions)
        || MetalSceneLinearRegionOpticsAccess::requested_core(*source_preimage) != source_rect
        || source_preimage->dimensions() != Dimensions{source_rect.width, source_rect.height}
        || MetalSceneLinearRegionOpticsAccess::full_dimensions(*source_preimage) != full_dimensions
        || source_preimage->completion_fence_value() == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal region optics RAW lease does not match the C-a preimage"
        );
    }
    if (!region.coordinate_remap() && source_rect != output_rect) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal pointwise region optics source and output rectangles differ"
        );
    }
    const std::size_t expected_gains = checked_rgb_bytes(
                                           source_rect.width,
                                           source_rect.height,
                                           "Metal region optics profile-gain dimensions overflow"
                                       )
                                       / sizeof(float);
    if (region.profile_vignetting()) {
        if (profile_vignetting_gains.size() != expected_gains
            || !std::all_of(
                profile_vignetting_gains.begin(),
                profile_vignetting_gains.end(),
                [](const float value) { return std::isfinite(value); }
            )) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Metal region optics profile gains do not match the source preimage"
            );
        }
    } else if (!profile_vignetting_gains.empty()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal region optics received gains for a disabled profile vignette"
        );
    }
}

void validate_preview_prepared_region(
    const MetalRawPreviewResidentOutput& source,
    const PreparedSceneLinearRegionOptics& optics,
    const lensfun_modifier_plan::PreparedRegion& region
) {
    const Dimensions full_dimensions = optics.full_dimensions();
    const GeometryPixelRect full_rect{
        .x = 0U,
        .y = 0U,
        .width = full_dimensions.width,
        .height = full_dimensions.height,
    };
    const GeometryPixelRect output_rect = region.output_rect();
    const auto& source_preimage = region.source_preimage();
    const auto& absolute_source_coordinates = region.absolute_source_coordinates();
    const auto& profile_vignetting_gains = region.profile_vignetting_gains();
    manual_optics::validate_settings(region.manual_output_settings());
    if (!optics.device_resident_eligible() || source.dimensions() != full_dimensions
        || !optics.owns_region(region) || output_rect != full_rect
        || region.executor_identity() != lensfun_modifier_plan::region_executor_identity) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Metal preview optics requires one compatible full-preview region"
        );
    }
    if (manual_optics::has_manual_geometry(region.manual_output_settings())
        || !source_preimage.has_value() || !rect_inside(*source_preimage, full_dimensions)) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "Metal preview optics requires a contained source preimage without manual geometry"
        );
    }
    const std::size_t output_pixels = checked_rgb_bytes(
                                          full_dimensions.width,
                                          full_dimensions.height,
                                          "Metal preview optics dimensions overflow"
                                      )
                                      / (3U * sizeof(float));
    if (region.coordinate_remap()) {
        if (absolute_source_coordinates.size() != output_pixels * 6U) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Metal preview optics coordinate table does not cover its full preview"
            );
        }
    } else if (!absolute_source_coordinates.empty()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal preview pointwise optics unexpectedly carries remap coordinates"
        );
    }
    if (source.row_stride_bytes()
            != static_cast<std::size_t>(full_dimensions.width) * 3U * sizeof(float)
        || source.output_bytes() < output_pixels * 3U * sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal preview optics requires a complete packed RGB source buffer"
        );
    }
    const std::size_t expected_gains = checked_rgb_bytes(
                                           source_preimage->width,
                                           source_preimage->height,
                                           "Metal preview optics gain dimensions overflow"
                                       )
                                       / sizeof(float);
    if (region.profile_vignetting()) {
        if (profile_vignetting_gains.size() != expected_gains
            || !std::all_of(
                profile_vignetting_gains.begin(),
                profile_vignetting_gains.end(),
                [](const float value) { return std::isfinite(value); }
            )) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Metal preview optics gains do not match its full preview"
            );
        }
    } else if (!profile_vignetting_gains.empty()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal preview optics received profile gains for a disabled stage"
        );
    }
}

[[nodiscard]] MetalSceneLinearRegionOpticsParameters make_parameters(
    const Dimensions full_dimensions,
    const lensfun_modifier_plan::PreparedRegion& region
) noexcept {
    const GeometryPixelRect output_rect = region.output_rect();
    const auto& source_preimage = region.source_preimage();
    const OpticsSettings& manual_settings = region.manual_output_settings();
    MetalSceneLinearRegionOpticsParameters parameters{
        .full_width = full_dimensions.width,
        .full_height = full_dimensions.height,
        .output_origin_x = output_rect.x,
        .output_origin_y = output_rect.y,
        .output_width = output_rect.width,
        .output_height = output_rect.height,
        .has_source = source_preimage.has_value() ? 1U : 0U,
        .coordinate_remap = region.coordinate_remap() ? 1U : 0U,
        .profile_vignetting = region.profile_vignetting() ? 1U : 0U,
        .manual_vignette_amount = static_cast<float>(
            static_cast<double>(manual_settings.manual_vignetting_amount) / 100.0
        ),
        .manual_vignette_midpoint = static_cast<float>(
            static_cast<double>(manual_settings.manual_vignetting_midpoint) / 100.0
        ),
    };
    if (source_preimage.has_value()) {
        parameters.source_origin_x = source_preimage->x;
        parameters.source_origin_y = source_preimage->y;
        parameters.source_width = source_preimage->width;
        parameters.source_height = source_preimage->height;
        parameters.source_buffer_origin_x = source_preimage->x;
        parameters.source_buffer_origin_y = source_preimage->y;
        parameters.source_buffer_width = source_preimage->width;
    }
    return parameters;
}

[[nodiscard]] std::string command_diagnostic(id<MTLCommandBuffer> command_buffer) {
    NSString* description = [command_buffer.error localizedDescription];
    const char* text = [description UTF8String];
    return text == nullptr || *text == '\0'
               ? "Metal scene-linear region optics command did not complete successfully"
               : std::string(text);
}

} // namespace

struct MetalSceneLinearRegionLease::Impl final {
    ~Impl() {
        [output_buffer release];
        [queue release];
        [device release];
    }

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLBuffer> output_buffer = nil;
    Dimensions dimensions;
    std::size_t output_bytes = 0U;
    std::uint64_t source_completion_fence = 0U;
    std::uint64_t completion_fence = 0U;
    std::uint64_t coordinate_upload_count = 0U;
    std::uint64_t coordinate_upload_bytes = 0U;
    std::uint64_t profile_gain_upload_count = 0U;
    std::uint64_t profile_gain_upload_bytes = 0U;
    bool source_slot_pinned_through_completion = false;
    std::atomic<std::uint64_t> debug_readback_count{0U};
    std::atomic<std::uint64_t> debug_readback_bytes{0U};
    std::atomic<bool> invalidated{false};
};

MetalSceneLinearRegionLease::MetalSceneLinearRegionLease(
    std::unique_ptr<Impl> implementation
) noexcept : implementation_(std::move(implementation)) {}

MetalSceneLinearRegionLease::MetalSceneLinearRegionLease(MetalSceneLinearRegionLease&&) noexcept =
    default;

MetalSceneLinearRegionLease&
MetalSceneLinearRegionLease::operator=(MetalSceneLinearRegionLease&&) noexcept = default;

MetalSceneLinearRegionLease::~MetalSceneLinearRegionLease() = default;

Dimensions MetalSceneLinearRegionLease::dimensions() const noexcept {
    return implementation_ == nullptr ? Dimensions{} : implementation_->dimensions;
}

std::uint64_t MetalSceneLinearRegionLease::device_identity() const noexcept {
    return implementation_ == nullptr
               ? 0U
               : static_cast<std::uint64_t>(implementation_->device.registryID);
}

std::uint64_t MetalSceneLinearRegionLease::retained_bytes() const noexcept {
    return implementation_ == nullptr ? 0U
                                      : static_cast<std::uint64_t>(implementation_->output_bytes);
}

std::uint64_t MetalSceneLinearRegionLease::completion_fence_value() const noexcept {
    return implementation_ == nullptr ? 0U : implementation_->completion_fence;
}

bool MetalSceneLinearRegionLease::valid() const noexcept {
    return implementation_ != nullptr
           && !implementation_->invalidated.load(std::memory_order_acquire);
}

MetalSceneLinearRegionOpticsTelemetry MetalSceneLinearRegionLease::telemetry() const noexcept {
    if (implementation_ == nullptr) {
        return {.invalidated = true};
    }
    return MetalSceneLinearRegionOpticsTelemetry{
        .source_reupload_count = 0U,
        .source_fp32_readback_count = 0U,
        .coordinate_upload_count = implementation_->coordinate_upload_count,
        .coordinate_upload_bytes = implementation_->coordinate_upload_bytes,
        .profile_gain_upload_count = implementation_->profile_gain_upload_count,
        .profile_gain_upload_bytes = implementation_->profile_gain_upload_bytes,
        .optics_dispatch_count = 1U,
        .debug_readback_count =
            implementation_->debug_readback_count.load(std::memory_order_relaxed),
        .debug_readback_bytes =
            implementation_->debug_readback_bytes.load(std::memory_order_relaxed),
        .source_completion_fence_value = implementation_->source_completion_fence,
        .completed_fence_value = implementation_->completion_fence,
        .source_slot_pinned_through_completion =
            implementation_->source_slot_pinned_through_completion,
        .invalidated = implementation_->invalidated.load(std::memory_order_acquire),
    };
}

SceneLinearRgbFrame MetalSceneLinearRegionLease::debug_readback() const {
    if (!valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "Metal scene-linear region lease is invalidated or moved"
        );
    }
    @autoreleasepool {
        OwnedObjectiveCObject readback_buffer([implementation_->device
            newBufferWithLength:implementation_->output_bytes
                        options:MTLResourceStorageModeShared]);
        id<MTLCommandBuffer> command_buffer = [implementation_->queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder =
            command_buffer == nil ? nil : [command_buffer blitCommandEncoder];
        if (!readback_buffer || encoder == nil) {
            implementation_->invalidated.store(true, std::memory_order_release);
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "Metal scene-linear region debug readback allocation failed"
            );
        }
        [encoder copyFromBuffer:implementation_->output_buffer
                   sourceOffset:0U
                       toBuffer:static_cast<id<MTLBuffer>>(readback_buffer.get())
              destinationOffset:0U
                           size:implementation_->output_bytes];
        [encoder endEncoding];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            implementation_->invalidated.store(true, std::memory_order_release);
            throw DecodeError(DecodeErrorCode::internal, 0, command_diagnostic(command_buffer));
        }
        SceneLinearRgbFrame output{
            .dimensions = implementation_->dimensions,
            .row_stride_bytes =
                static_cast<std::size_t>(implementation_->dimensions.width) * 3U * sizeof(float),
            .samples = std::vector<float>(implementation_->output_bytes / sizeof(float)),
        };
        std::memcpy(
            output.samples.data(),
            [static_cast<id<MTLBuffer>>(readback_buffer.get()) contents],
            implementation_->output_bytes
        );
        implementation_->debug_readback_count.fetch_add(1U, std::memory_order_relaxed);
        implementation_->debug_readback_bytes.fetch_add(
            static_cast<std::uint64_t>(implementation_->output_bytes),
            std::memory_order_relaxed
        );
        return output;
    }
}

void* MetalSceneLinearRegionLease::native_buffer_handle() const noexcept {
    return implementation_ == nullptr ? nullptr
                                      : reinterpret_cast<void*>(implementation_->output_buffer);
}

void* MetalSceneLinearRegionLease::native_device_handle() const noexcept {
    return implementation_ == nullptr ? nullptr : reinterpret_cast<void*>(implementation_->device);
}

void* MetalSceneLinearRegionLease::native_queue_handle() const noexcept {
    return implementation_ == nullptr ? nullptr : reinterpret_cast<void*>(implementation_->queue);
}

MetalSceneLinearRegionLease develop_metal_scene_linear_region_optics(
    const raw_pipeline_detail::ResidentRawSource& source,
    lensfun_modifier_plan::PreparedRegion region,
    const std::uint64_t source_resident_allowance_bytes
) {
    if (!source.device_path_valid()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal region optics requires a valid owner-bound resident RAW source"
        );
    }
    const PreparedSceneLinearRegionOptics& optics = source.region_optics();
    if (source.dimensions() != optics.full_dimensions() || !optics.owns_region(region)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal region optics evidence does not belong to this resident RAW session"
        );
    }
    const raw_pipeline_detail::MetalResidentRawSource& raw_source = source.metal_source();
    std::optional<raw_pipeline_detail::MetalResidentRawRegionLease> source_preimage;
    if (region.source_preimage().has_value()) {
        source_preimage.emplace(raw_source.develop_device_region(
            *region.source_preimage(),
            source_resident_allowance_bytes
        ));
    }
    validate_prepared_region(source_preimage, optics, region);
    const Dimensions full_dimensions = optics.full_dimensions();
    const GeometryPixelRect output_rect = region.output_rect();
    const auto& absolute_source_coordinates = region.absolute_source_coordinates();
    const auto& profile_vignetting_gains = region.profile_vignetting_gains();
    auto& runtime = context();
    if (!runtime.available()) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            runtime.diagnostic_.empty() ? "Metal scene-linear region optics is unavailable"
                                        : runtime.diagnostic_
        );
    }

    id<MTLDevice> source_device = source_preimage.has_value()
                                      ? MetalSceneLinearRegionOpticsAccess::device(*source_preimage)
                                      : runtime.device_;
    if (source_device == nil
        || static_cast<std::uint64_t>(source_device.registryID)
               != static_cast<std::uint64_t>(runtime.device_.registryID)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal RAW and region-optics resources belong to different devices"
        );
    }
    const auto invalidate_source = [&]() noexcept {
        if (source_preimage.has_value()) {
            MetalSceneLinearRegionOpticsAccess::invalidate(*source_preimage);
        }
    };
    const std::size_t output_bytes = checked_rgb_bytes(
        output_rect.width,
        output_rect.height,
        "Metal region optics output dimensions overflow"
    );
    if (output_bytes > static_cast<std::size_t>(runtime.device_.maxBufferLength)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "Metal region optics output exceeds this device's buffer limit"
        );
    }

    bool admitted = true;
    try {
        if (environment_enabled("SHADOW_TEST_METAL_SCENE_LINEAR_OPTICS_FAIL")) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "test-forced Metal scene-linear region optics failure"
            );
        }
        @autoreleasepool {
            constexpr std::array<float, 2U> dummy_coordinates{0.0F, 0.0F};
            constexpr std::array<float, 1U> dummy_gain{1.0F};
            constexpr std::array<float, 1U> dummy_source{0.0F};

            const bool has_coordinates = !absolute_source_coordinates.empty();
            const bool has_profile_gains = !profile_vignetting_gains.empty();
            const std::size_t coordinate_bytes =
                has_coordinates ? checked_float_bytes(
                                      absolute_source_coordinates.size(),
                                      "Metal region optics coordinate upload is too large"
                                  )
                                : sizeof(dummy_coordinates);
            const std::size_t gain_bytes =
                has_profile_gains ? checked_float_bytes(
                                        profile_vignetting_gains.size(),
                                        "Metal region optics profile-gain upload is too large"
                                    )
                                  : sizeof(dummy_gain);
            if (coordinate_bytes > static_cast<std::size_t>(runtime.device_.maxBufferLength)
                || gain_bytes > static_cast<std::size_t>(runtime.device_.maxBufferLength)) {
                throw DecodeError(
                    DecodeErrorCode::resource_limit,
                    0,
                    "Metal region optics evidence exceeds this device's buffer limit"
                );
            }
            OwnedObjectiveCObject coordinate_buffer([runtime.device_
                newBufferWithBytes:has_coordinates
                                       ? static_cast<const void*>(
                                             absolute_source_coordinates.data()
                                         )
                                       : static_cast<const void*>(dummy_coordinates.data())
                            length:coordinate_bytes
                           options:MTLResourceStorageModeShared]);
            OwnedObjectiveCObject gain_buffer([runtime.device_
                newBufferWithBytes:has_profile_gains
                                       ? static_cast<const void*>(profile_vignetting_gains.data())
                                       : static_cast<const void*>(dummy_gain.data())
                            length:gain_bytes
                           options:MTLResourceStorageModeShared]);
            OwnedObjectiveCObject dummy_source_buffer;
            id<MTLBuffer> source_buffer = nil;
            if (source_preimage.has_value()) {
                source_buffer = MetalSceneLinearRegionOpticsAccess::buffer(*source_preimage);
            } else {
                dummy_source_buffer = OwnedObjectiveCObject([runtime.device_
                    newBufferWithBytes:dummy_source.data()
                                length:sizeof(dummy_source)
                               options:MTLResourceStorageModeShared]);
                source_buffer = static_cast<id<MTLBuffer>>(dummy_source_buffer.get());
            }
            OwnedObjectiveCObject output_buffer([runtime.device_
                newBufferWithLength:output_bytes
                            options:MTLResourceStorageModePrivate]);
            OwnedObjectiveCObject failure_buffer([runtime.device_
                newBufferWithLength:sizeof(std::uint32_t)
                            options:MTLResourceStorageModeShared]);
            if (!coordinate_buffer || !gain_buffer || source_buffer == nil || !output_buffer
                || !failure_buffer) {
                throw DecodeError(
                    DecodeErrorCode::resource_limit,
                    0,
                    "Metal scene-linear region optics could not allocate bounded resources"
                );
            }
            auto* failure =
                static_cast<std::uint32_t*>([static_cast<id<MTLBuffer>>(failure_buffer.get())
                    contents]);
            *failure = 0U;
            const MetalSceneLinearRegionOpticsParameters parameters =
                make_parameters(full_dimensions, region);
            id<MTLCommandBuffer> command_buffer = [runtime.queue_ commandBuffer];
            id<MTLComputeCommandEncoder> encoder =
                command_buffer == nil ? nil : [command_buffer computeCommandEncoder];
            if (encoder == nil) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Metal scene-linear region optics could not create a compute command"
                );
            }
            const NSUInteger thread_width = std::min<NSUInteger>(
                32U,
                std::max<NSUInteger>(1U, runtime.pipeline_.threadExecutionWidth)
            );
            const NSUInteger thread_height = std::max<NSUInteger>(
                1U,
                std::min<NSUInteger>(
                    8U,
                    runtime.pipeline_.maxTotalThreadsPerThreadgroup / thread_width
                )
            );
            [encoder setComputePipelineState:runtime.pipeline_];
            [encoder setBuffer:source_buffer offset:0U atIndex:0U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(coordinate_buffer.get())
                        offset:0U
                       atIndex:1U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(gain_buffer.get()) offset:0U atIndex:2U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(output_buffer.get())
                        offset:0U
                       atIndex:3U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(failure_buffer.get())
                        offset:0U
                       atIndex:4U];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:5U];
            [encoder dispatchThreads:MTLSizeMake(output_rect.width, output_rect.height, 1U)
                threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
            [encoder endEncoding];

            const std::uint64_t fence_value =
                runtime.next_fence_value_.fetch_add(1U, std::memory_order_relaxed);
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                throw DecodeError(DecodeErrorCode::internal, 0, command_diagnostic(command_buffer));
            }
            if (*failure != 0U) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Metal scene-linear region optics detected invalid source evidence or "
                    "non-finite output"
                );
            }

            auto implementation = std::make_unique<MetalSceneLinearRegionLease::Impl>();
            implementation->device = [runtime.device_ retain];
            implementation->queue = [runtime.queue_ retain];
            implementation->output_buffer =
                [static_cast<id<MTLBuffer>>(output_buffer.get()) retain];
            implementation->dimensions = {
                output_rect.width,
                output_rect.height,
            };
            implementation->output_bytes = output_bytes;
            implementation->source_completion_fence =
                source_preimage.has_value() ? source_preimage->completion_fence_value() : 0U;
            implementation->completion_fence = fence_value;
            implementation->coordinate_upload_count = has_coordinates ? 1U : 0U;
            implementation->coordinate_upload_bytes =
                has_coordinates ? static_cast<std::uint64_t>(coordinate_bytes) : 0U;
            implementation->profile_gain_upload_count = has_profile_gains ? 1U : 0U;
            implementation->profile_gain_upload_bytes =
                has_profile_gains ? static_cast<std::uint64_t>(gain_bytes) : 0U;
            implementation->source_slot_pinned_through_completion = source_preimage.has_value();

            // The moved RAW lease remains alive until this command has completed and the output
            // buffer has been retained. Its destruction now releases the source slot.
            admitted = false;
            return MetalSceneLinearRegionLease(std::move(implementation));
        }
    } catch (...) {
        if (admitted) {
            invalidate_source();
        }
        throw;
    }
}

MetalSceneLinearRegionLease apply_metal_scene_linear_preview_optics(
    const MetalRawPreviewResidentOutput& source,
    const PreparedSceneLinearRegionOptics& optics,
    lensfun_modifier_plan::PreparedRegion region
) {
    validate_preview_prepared_region(source, optics, region);
    auto& runtime = context();
    if (!runtime.available()) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            runtime.diagnostic_.empty() ? "Metal scene-linear preview optics is unavailable"
                                        : runtime.diagnostic_
        );
    }
    @autoreleasepool {
        id<MTLDevice> source_device = static_cast<id<MTLDevice>>(source.native_device_handle());
        id<MTLBuffer> source_buffer = static_cast<id<MTLBuffer>>(source.native_buffer_handle());
        if (source_device == nil || source_buffer == nil
            || static_cast<std::uint64_t>(source_device.registryID)
                   != static_cast<std::uint64_t>(runtime.device_.registryID)) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Metal preview optics source does not belong to the active device"
            );
        }
        if (environment_enabled("SHADOW_TEST_METAL_SCENE_LINEAR_OPTICS_FAIL")) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "test-forced Metal scene-linear preview optics failure"
            );
        }

        constexpr std::array<float, 2U> dummy_coordinates{0.0F, 0.0F};
        constexpr std::array<float, 1U> dummy_gain{1.0F};
        const auto& absolute_source_coordinates = region.absolute_source_coordinates();
        const auto& profile_vignetting_gains = region.profile_vignetting_gains();
        const bool has_coordinates = !absolute_source_coordinates.empty();
        const bool has_profile_gains = !profile_vignetting_gains.empty();
        const std::size_t coordinate_bytes =
            has_coordinates ? checked_float_bytes(
                                  absolute_source_coordinates.size(),
                                  "Metal preview optics coordinate upload is too large"
                              )
                            : sizeof(dummy_coordinates);
        const std::size_t gain_bytes = has_profile_gains
                                           ? checked_float_bytes(
                                                 profile_vignetting_gains.size(),
                                                 "Metal preview optics gain upload is too large"
                                             )
                                           : sizeof(dummy_gain);
        const std::size_t output_bytes = checked_rgb_bytes(
            source.dimensions().width,
            source.dimensions().height,
            "Metal preview optics output dimensions overflow"
        );
        if (coordinate_bytes > static_cast<std::size_t>(runtime.device_.maxBufferLength)
            || gain_bytes > static_cast<std::size_t>(runtime.device_.maxBufferLength)
            || output_bytes > static_cast<std::size_t>(runtime.device_.maxBufferLength)) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "Metal preview optics evidence exceeds this device's buffer limit"
            );
        }
        OwnedObjectiveCObject coordinate_buffer([runtime.device_
            newBufferWithBytes:has_coordinates
                                   ? static_cast<const void*>(absolute_source_coordinates.data())
                                   : static_cast<const void*>(dummy_coordinates.data())
                        length:coordinate_bytes
                       options:MTLResourceStorageModeShared]);
        OwnedObjectiveCObject gain_buffer([runtime.device_
            newBufferWithBytes:has_profile_gains
                                   ? static_cast<const void*>(profile_vignetting_gains.data())
                                   : static_cast<const void*>(dummy_gain.data())
                        length:gain_bytes
                       options:MTLResourceStorageModeShared]);
        OwnedObjectiveCObject output_buffer([runtime.device_
            newBufferWithLength:output_bytes
                        options:MTLResourceStorageModePrivate]);
        OwnedObjectiveCObject failure_buffer([runtime.device_
            newBufferWithLength:sizeof(std::uint32_t)
                        options:MTLResourceStorageModeShared]);
        if (!coordinate_buffer || !gain_buffer || !output_buffer || !failure_buffer) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "Metal preview optics could not allocate bounded resources"
            );
        }
        auto* failure =
            static_cast<std::uint32_t*>([static_cast<id<MTLBuffer>>(failure_buffer.get())
                contents]);
        *failure = 0U;
        auto parameters = make_parameters(optics.full_dimensions(), region);
        parameters.source_buffer_origin_x = 0U;
        parameters.source_buffer_origin_y = 0U;
        parameters.source_buffer_width = source.dimensions().width;
        id<MTLCommandBuffer> command_buffer = [runtime.queue_ commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            command_buffer == nil ? nil : [command_buffer computeCommandEncoder];
        if (encoder == nil) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "Metal preview optics could not create a compute command"
            );
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
        [encoder setBuffer:source_buffer offset:0U atIndex:0U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(coordinate_buffer.get())
                    offset:0U
                   atIndex:1U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(gain_buffer.get()) offset:0U atIndex:2U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(output_buffer.get()) offset:0U atIndex:3U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(failure_buffer.get()) offset:0U atIndex:4U];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:5U];
        [encoder dispatchThreads:MTLSizeMake(
                                     source.dimensions().width,
                                     source.dimensions().height,
                                     1U
                                 )
            threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
        [encoder endEncoding];

        const std::uint64_t fence_value =
            runtime.next_fence_value_.fetch_add(1U, std::memory_order_relaxed);
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            throw DecodeError(DecodeErrorCode::internal, 0, command_diagnostic(command_buffer));
        }
        if (*failure != 0U) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "Metal preview optics detected invalid source evidence or non-finite output"
            );
        }
        auto implementation = std::make_unique<MetalSceneLinearRegionLease::Impl>();
        implementation->device = [runtime.device_ retain];
        implementation->queue = [runtime.queue_ retain];
        implementation->output_buffer = [static_cast<id<MTLBuffer>>(output_buffer.get()) retain];
        implementation->dimensions = source.dimensions();
        implementation->output_bytes = output_bytes;
        implementation->completion_fence = fence_value;
        implementation->coordinate_upload_count = has_coordinates ? 1U : 0U;
        implementation->coordinate_upload_bytes =
            has_coordinates ? static_cast<std::uint64_t>(coordinate_bytes) : 0U;
        implementation->profile_gain_upload_count = has_profile_gains ? 1U : 0U;
        implementation->profile_gain_upload_bytes =
            has_profile_gains ? static_cast<std::uint64_t>(gain_bytes) : 0U;
        return MetalSceneLinearRegionLease(std::move(implementation));
    }
}

bool metal_scene_linear_region_optics_available() noexcept {
    return context().available();
}

const std::string& metal_scene_linear_region_optics_diagnostic() noexcept {
    return context().diagnostic_;
}

} // namespace shadow::image::detail
