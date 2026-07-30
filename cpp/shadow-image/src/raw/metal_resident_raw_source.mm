#include "metal_resident_raw_source.hpp"

#include "metal_dcp_color_encoding.hpp"
#include "metal_raw_denoise_encoding.hpp"
#include "metal_raw_runtime.hpp"
#include "raw_denoise_plan.hpp"
#include "resident_raw_source.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::image::raw_pipeline_detail {

namespace {

struct ResidentRawDevelopmentParameters final {
    std::uint32_t storage_width = 0U;
    std::uint32_t storage_height = 0U;
    std::uint32_t active_width = 0U;
    std::uint32_t active_height = 0U;
    std::uint32_t margin_left = 0U;
    std::uint32_t margin_top = 0U;
    std::uint32_t output_width = 0U;
    std::uint32_t output_height = 0U;
    std::uint32_t reconstruction_width = 0U;
    std::uint32_t reconstruction_height = 0U;
    std::int32_t orientation = 0;
    std::uint32_t output_row_offset = 0U;
    std::uint32_t output_tile_height = 0U;
    std::uint32_t neutralize_sensor_highlights = 0U;
    std::uint32_t project_sensor_clipping = 0U;
    std::uint32_t reconstruction_quality = 0U;
    std::uint32_t cfa_channels[4]{};
    float black_levels[4]{};
    float white_minus_black[4]{};
    float camera_to_linear_srgb[9]{};
};

static_assert(sizeof(ResidentRawDevelopmentParameters) == 148U);
static_assert(offsetof(ResidentRawDevelopmentParameters, storage_width) == 0U);
static_assert(offsetof(ResidentRawDevelopmentParameters, reconstruction_width) == 32U);
static_assert(offsetof(ResidentRawDevelopmentParameters, orientation) == 40U);
static_assert(offsetof(ResidentRawDevelopmentParameters, neutralize_sensor_highlights) == 52U);
static_assert(offsetof(ResidentRawDevelopmentParameters, reconstruction_quality) == 60U);
static_assert(offsetof(ResidentRawDevelopmentParameters, cfa_channels) == 64U);
static_assert(offsetof(ResidentRawDevelopmentParameters, camera_to_linear_srgb) == 112U);

[[nodiscard]] bool environment_enabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

[[nodiscard]] std::size_t region_buffer_limit(id<MTLDevice> device) noexcept {
    std::size_t limit = static_cast<std::size_t>(device.maxBufferLength);
    const char* override_value =
        std::getenv("SHADOW_TEST_METAL_RESIDENT_REGION_BUFFER_LIMIT_BYTES");
    if (override_value == nullptr || *override_value == '\0') {
        return limit;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(override_value, &end, 10);
    if (errno != 0 || end == override_value || *end != '\0' || parsed == 0U) {
        return limit;
    }
    return std::min<std::size_t>(limit, static_cast<std::size_t>(parsed));
}

[[nodiscard]] std::uint32_t cfa_channel(const RawCfaColor color) {
    switch (color) {
    case RawCfaColor::red:
        return 0U;
    case RawCfaColor::green:
        return 1U;
    case RawCfaColor::blue:
        return 2U;
    case RawCfaColor::unknown:
        break;
    }
    throw DecodeError(
        DecodeErrorCode::unsupported_layout,
        0,
        "Metal resident RAW source encountered an unknown CFA colour"
    );
}

[[nodiscard]] Dimensions
oriented_dimensions(const Dimensions dimensions, const std::int32_t orientation) noexcept {
    return orientation == 5 || orientation == 6 ? Dimensions{dimensions.height, dimensions.width}
                                                : dimensions;
}

[[nodiscard]] bool rect_inside(const GeometryPixelRect rect, const Dimensions dimensions) noexcept {
    return rect.width != 0U && rect.height != 0U && rect.x < dimensions.width
           && rect.y < dimensions.height && rect.width <= dimensions.width - rect.x
           && rect.height <= dimensions.height - rect.y;
}

[[nodiscard]] std::size_t checked_sensor_bytes(const RawFrame& frame) {
    std::size_t bytes = 0U;
    if (!detail::checked_multiply(frame.samples.size(), sizeof(std::uint16_t), bytes)
        || bytes == 0U) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "Metal resident RAW sensor byte count overflows"
        );
    }
    return bytes;
}

[[nodiscard]] std::size_t checked_region_bytes(const GeometryPixelRect rect) {
    std::size_t pixel_count = 0U;
    std::size_t sample_count = 0U;
    std::size_t bytes = 0U;
    if (!detail::checked_multiply(
            static_cast<std::size_t>(rect.width),
            static_cast<std::size_t>(rect.height),
            pixel_count
        )
        || !detail::checked_multiply(pixel_count, 3U, sample_count)
        || !detail::checked_multiply(sample_count, sizeof(float), bytes) || bytes == 0U) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "Metal resident RAW region byte count overflows"
        );
    }
    return bytes;
}

[[nodiscard]] ResidentRawDevelopmentParameters make_parameters(
    const RawFrameDescriptor& descriptor,
    const RawFrameLinearTransform& transform,
    const RawDevelopmentPlan& plan,
    const GeometryPixelRect region
) {
    ResidentRawDevelopmentParameters parameters;
    parameters.storage_width = descriptor.storage_dimensions.width;
    parameters.storage_height = descriptor.storage_dimensions.height;
    parameters.active_width = descriptor.active_dimensions.width;
    parameters.active_height = descriptor.active_dimensions.height;
    parameters.margin_left = descriptor.active_margins.left;
    parameters.margin_top = descriptor.active_margins.top;
    parameters.output_width = region.width;
    parameters.output_height = region.height;
    parameters.reconstruction_width = descriptor.active_dimensions.width;
    parameters.reconstruction_height = descriptor.active_dimensions.height;
    parameters.orientation = descriptor.orientation;
    parameters.output_tile_height = region.height;
    parameters.neutralize_sensor_highlights =
        plan.highlight_recovery == RawHighlightRecoveryIntent::provider_default ? 1U : 0U;
    parameters.reconstruction_quality = static_cast<std::uint32_t>(plan.quality);
    for (std::size_t site = 0U; site < 4U; ++site) {
        parameters.cfa_channels[site] = cfa_channel(descriptor.bayer_2x2[site]);
        parameters.black_levels[site] = static_cast<float>(descriptor.black_levels[site]);
        parameters.white_minus_black[site] =
            static_cast<float>(descriptor.white_levels[site] - descriptor.black_levels[site]);
    }
    for (std::size_t index = 0U; index < 9U; ++index) {
        parameters.camera_to_linear_srgb[index] =
            static_cast<float>(transform.camera_to_linear_srgb_d65[index]);
    }
    return parameters;
}

[[nodiscard]] SceneLinearRgbFrame allocate_region_output(const GeometryPixelRect region) {
    const std::size_t bytes = checked_region_bytes(region);
    SceneLinearRgbFrame output;
    output.dimensions = {region.width, region.height};
    output.row_stride_bytes = static_cast<std::size_t>(region.width) * 3U * sizeof(float);
    output.samples.resize(bytes / sizeof(float));
    return output;
}

void store_completed_fence(
    std::atomic<std::uint64_t>& completed,
    const std::uint64_t value
) noexcept {
    std::uint64_t observed = completed.load(std::memory_order_relaxed);
    while (observed < value
           && !completed.compare_exchange_weak(
               observed,
               value,
               std::memory_order_release,
               std::memory_order_relaxed
           )) {}
}

void store_maximum(std::atomic<std::uint64_t>& maximum, const std::uint64_t value) noexcept {
    std::uint64_t observed = maximum.load(std::memory_order_relaxed);
    while (observed < value
           && !maximum.compare_exchange_weak(
               observed,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed
           )) {}
}

} // namespace

struct MetalResidentRawSource::Impl final {
    struct OutputSlot final {
        id<MTLBuffer> buffer = nil;
        std::size_t capacity_bytes = 0U;
        bool in_use = false;
    };

    ~Impl() {
        for (auto& slot : output_slots) {
            [slot.buffer release];
        }
        [resident_samples release];
        [pipeline release];
        [queue release];
        [device release];
    }

    [[nodiscard]] std::size_t acquire_output_slot(
        const std::size_t required_bytes,
        const std::uint64_t source_resident_allowance_bytes
    ) {
        std::unique_lock lock(output_slots_mutex);
        output_slots_ready.wait(lock, [this] {
            return invalidated.load(std::memory_order_acquire)
                   || std::any_of(
                       output_slots.begin(),
                       output_slots.end(),
                       [](const OutputSlot& slot) { return !slot.in_use; }
                   );
        });
        if (invalidated.load(std::memory_order_acquire)) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "Metal resident RAW source is invalidated"
            );
        }
        auto iterator = output_slots.end();
        for (auto candidate = output_slots.begin(); candidate != output_slots.end(); ++candidate) {
            if (!candidate->in_use && candidate->capacity_bytes >= required_bytes
                && (iterator == output_slots.end()
                    || candidate->capacity_bytes < iterator->capacity_bytes)) {
                iterator = candidate;
            }
        }
        if (iterator == output_slots.end()) {
            for (auto candidate = output_slots.begin(); candidate != output_slots.end();
                 ++candidate) {
                if (!candidate->in_use
                    && (iterator == output_slots.end()
                        || candidate->capacity_bytes > iterator->capacity_bytes)) {
                    iterator = candidate;
                }
            }
        }
        const std::size_t index =
            static_cast<std::size_t>(std::distance(output_slots.begin(), iterator));
        iterator->in_use = true;
        if (iterator->capacity_bytes < required_bytes) {
            // A replacement buffer is a new live allocation, not merely the final net growth.
            // Release the idle old slot before admission/allocation so retained-byte accounting
            // remains an actual upper bound throughout the transaction.
            const std::size_t old_capacity = iterator->capacity_bytes;
            id<MTLBuffer> old_buffer = iterator->buffer;
            iterator->buffer = nil;
            iterator->capacity_bytes = 0U;
            if (old_buffer != nil) {
                [old_buffer release];
                retained_device_bytes.fetch_sub(
                    static_cast<std::uint64_t>(old_capacity),
                    std::memory_order_relaxed
                );
            }
            const std::uint64_t retained_bytes =
                retained_device_bytes.load(std::memory_order_relaxed);
            if (retained_bytes > source_resident_allowance_bytes
                || static_cast<std::uint64_t>(required_bytes)
                       > source_resident_allowance_bytes - retained_bytes) {
                iterator->in_use = false;
                output_slots_ready.notify_one();
                throw DecodeError(
                    DecodeErrorCode::resource_limit,
                    0,
                    "Metal resident RAW output slot exceeds the transaction allowance"
                );
            }
            if (old_capacity != 0U
                && environment_enabled("SHADOW_TEST_METAL_RESIDENT_FAIL_SLOT_REPLACEMENT")) {
                iterator->in_use = false;
                output_slots_ready.notify_one();
                throw DecodeError(
                    DecodeErrorCode::resource_limit,
                    0,
                    "test-forced Metal resident RAW output-slot replacement failure"
                );
            }
            id<MTLBuffer> replacement = [device newBufferWithLength:required_bytes
                                                            options:MTLResourceStorageModeShared];
            if (replacement == nil) {
                iterator->in_use = false;
                output_slots_ready.notify_one();
                throw DecodeError(
                    DecodeErrorCode::resource_limit,
                    0,
                    "Metal resident RAW source could not allocate a bounded output slot"
                );
            }
            iterator->buffer = replacement;
            iterator->capacity_bytes = required_bytes;
            retained_device_bytes.fetch_add(
                static_cast<std::uint64_t>(required_bytes),
                std::memory_order_relaxed
            );
        }
        return index;
    }

    void release_output_slot(const std::size_t index) noexcept {
        {
            std::lock_guard lock(output_slots_mutex);
            output_slots[index].in_use = false;
        }
        output_slots_ready.notify_one();
    }

    void invalidate() noexcept {
        bool expected = false;
        if (invalidated.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            invalidation_count.fetch_add(1U, std::memory_order_relaxed);
            output_slots_ready.notify_all();
        }
    }

    RawFrameDescriptor descriptor;
    RawFrameLinearTransform transform;
    RawDevelopmentPlan development_plan;
    RawBayerDenoiseReceipt raw_denoise_receipt;
    std::unique_ptr<detail::MetalDcpColorEncoding> dcp_encoding;
    std::size_t dcp_resource_bytes = 0U;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLBuffer> resident_samples = nil;
    std::array<OutputSlot, 2U> output_slots{};
    mutable std::mutex output_slots_mutex;
    mutable std::condition_variable output_slots_ready;
    std::atomic<std::uint64_t> source_upload_count{1U};
    std::atomic<std::uint64_t> source_upload_bytes{0U};
    std::atomic<std::uint64_t> denoise_dispatch_count{0U};
    mutable std::atomic<std::uint64_t> region_dispatch_count{0U};
    mutable std::atomic<std::uint64_t> region_readback_count{0U};
    mutable std::atomic<std::uint64_t> region_readback_bytes{0U};
    std::atomic<std::uint64_t> source_buffer_release_count{1U};
    mutable std::atomic<std::uint64_t> region_lease_count{0U};
    mutable std::atomic<std::uint64_t> region_lease_release_count{0U};
    mutable std::atomic<std::uint64_t> active_region_lease_count{0U};
    mutable std::atomic<std::uint64_t> maximum_active_region_lease_count{0U};
    std::atomic<std::uint64_t> invalidation_count{0U};
    mutable std::atomic<std::uint64_t> next_fence_value{1U};
    mutable std::atomic<std::uint64_t> completed_fence_value{0U};
    std::atomic<std::uint64_t> retained_device_bytes{0U};
    std::atomic<bool> invalidated{false};
};

struct MetalResidentRawRegionLease::Impl final {
    Impl(
        std::shared_ptr<MetalResidentRawSource::Impl> source_implementation,
        const std::size_t output_slot_index,
        const GeometryPixelRect core,
        const std::size_t bytes,
        const std::uint64_t fence
    ) :
        source(std::move(source_implementation)), slot_index(output_slot_index),
        requested_core(core), retained_bytes(bytes), completion_fence(fence) {
        source->region_lease_count.fetch_add(1U, std::memory_order_relaxed);
        const std::uint64_t active =
            source->active_region_lease_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
        store_maximum(source->maximum_active_region_lease_count, active);
    }

    ~Impl() {
        source->release_output_slot(slot_index);
        source->active_region_lease_count.fetch_sub(1U, std::memory_order_relaxed);
        source->region_lease_release_count.fetch_add(1U, std::memory_order_relaxed);
    }

    std::shared_ptr<MetalResidentRawSource::Impl> source;
    std::size_t slot_index = 0U;
    GeometryPixelRect requested_core;
    std::size_t retained_bytes = 0U;
    std::uint64_t completion_fence = 0U;
};

MetalResidentRawRegionLease::MetalResidentRawRegionLease(
    std::unique_ptr<Impl> implementation
) noexcept : implementation_(std::move(implementation)) {}

MetalResidentRawRegionLease::MetalResidentRawRegionLease(MetalResidentRawRegionLease&&) noexcept =
    default;

MetalResidentRawRegionLease&
MetalResidentRawRegionLease::operator=(MetalResidentRawRegionLease&&) noexcept = default;

MetalResidentRawRegionLease::~MetalResidentRawRegionLease() = default;

Dimensions MetalResidentRawRegionLease::dimensions() const noexcept {
    return implementation_ == nullptr ? Dimensions{}
                                      : Dimensions{
                                            implementation_->requested_core.width,
                                            implementation_->requested_core.height,
                                        };
}

std::uint64_t MetalResidentRawRegionLease::device_identity() const noexcept {
    return implementation_ == nullptr
               ? 0U
               : static_cast<std::uint64_t>(implementation_->source->device.registryID);
}

std::uint64_t MetalResidentRawRegionLease::retained_bytes() const noexcept {
    return implementation_ == nullptr ? 0U
                                      : static_cast<std::uint64_t>(implementation_->retained_bytes);
}

std::uint64_t MetalResidentRawRegionLease::completion_fence_value() const noexcept {
    return implementation_ == nullptr ? 0U : implementation_->completion_fence;
}

DevelopedMetalResidentRawRegion MetalResidentRawRegionLease::readback_compatibility() const {
    if (implementation_ == nullptr) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "Metal resident RAW region lease has already been moved"
        );
    }
    SceneLinearRgbFrame output = allocate_region_output(implementation_->requested_core);
    std::memcpy(
        output.samples.data(),
        [implementation_->source->output_slots[implementation_->slot_index].buffer contents],
        implementation_->retained_bytes
    );
    implementation_->source->region_readback_count.fetch_add(1U, std::memory_order_relaxed);
    implementation_->source->region_readback_bytes.fetch_add(
        static_cast<std::uint64_t>(implementation_->retained_bytes),
        std::memory_order_relaxed
    );
    return DevelopedMetalResidentRawRegion{
        .requested_core = implementation_->requested_core,
        .scene_linear = std::move(output),
    };
}

void* MetalResidentRawRegionLease::native_buffer_handle() const noexcept {
    return implementation_ == nullptr
               ? nullptr
               : reinterpret_cast<void*>(
                     implementation_->source->output_slots[implementation_->slot_index].buffer
                 );
}

void* MetalResidentRawRegionLease::native_device_handle() const noexcept {
    return implementation_ == nullptr ? nullptr
                                      : reinterpret_cast<void*>(implementation_->source->device);
}

GeometryPixelRect MetalResidentRawRegionLease::requested_core() const noexcept {
    return implementation_ == nullptr ? GeometryPixelRect{} : implementation_->requested_core;
}

Dimensions MetalResidentRawRegionLease::full_dimensions() const noexcept {
    return implementation_ == nullptr ? Dimensions{}
                                      : oriented_dimensions(
                                            implementation_->source->descriptor.active_dimensions,
                                            implementation_->source->descriptor.orientation
                                        );
}

void MetalResidentRawRegionLease::invalidate_source() const noexcept {
    if (implementation_ != nullptr) {
        implementation_->source->invalidate();
    }
}

MetalResidentRawSource::MetalResidentRawSource(std::shared_ptr<Impl> implementation) noexcept :
    implementation_(std::move(implementation)) {}

MetalResidentRawSource::MetalResidentRawSource(MetalResidentRawSource&&) noexcept = default;

MetalResidentRawSource&
MetalResidentRawSource::operator=(MetalResidentRawSource&&) noexcept = default;

MetalResidentRawSource::~MetalResidentRawSource() = default;

Dimensions MetalResidentRawSource::dimensions() const noexcept {
    return oriented_dimensions(
        implementation_->descriptor.active_dimensions,
        implementation_->descriptor.orientation
    );
}

std::uint64_t MetalResidentRawSource::retained_bytes() const noexcept {
    return implementation_->retained_device_bytes.load(std::memory_order_relaxed);
}

const RawBayerDenoiseReceipt& MetalResidentRawSource::raw_denoise_receipt() const noexcept {
    return implementation_->raw_denoise_receipt;
}

RawDemosaicReceipt MetalResidentRawSource::demosaic_receipt() const noexcept {
    return RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = implementation_->descriptor.schema_version,
        .algorithm = implementation_->development_plan.quality == RawDevelopmentQuality::high
                         ? RawDemosaicAlgorithm::bayer_edge_aware_v1
                         : RawDemosaicAlgorithm::bayer_bilinear_v1,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = false,
        .dng_opcodes_applied = false,
    };
}

bool MetalResidentRawSource::dcp_applied() const noexcept {
    return implementation_->dcp_encoding != nullptr;
}

bool MetalResidentRawSource::valid() const noexcept {
    return !implementation_->invalidated.load(std::memory_order_acquire);
}

void MetalResidentRawSource::invalidate() const noexcept {
    implementation_->invalidate();
}

MetalResidentRawSourceTelemetry MetalResidentRawSource::telemetry() const noexcept {
    return MetalResidentRawSourceTelemetry{
        .source_upload_count = implementation_->source_upload_count.load(std::memory_order_relaxed),
        .source_upload_bytes = implementation_->source_upload_bytes.load(std::memory_order_relaxed),
        .denoise_dispatch_count =
            implementation_->denoise_dispatch_count.load(std::memory_order_relaxed),
        .region_dispatch_count =
            implementation_->region_dispatch_count.load(std::memory_order_relaxed),
        .region_readback_count =
            implementation_->region_readback_count.load(std::memory_order_relaxed),
        .region_readback_bytes =
            implementation_->region_readback_bytes.load(std::memory_order_relaxed),
        .full_frame_readback_count = 0U,
        .full_frame_readback_bytes = 0U,
        .source_buffer_release_count =
            implementation_->source_buffer_release_count.load(std::memory_order_relaxed),
        .region_lease_count = implementation_->region_lease_count.load(std::memory_order_relaxed),
        .region_lease_release_count =
            implementation_->region_lease_release_count.load(std::memory_order_relaxed),
        .active_region_lease_count =
            implementation_->active_region_lease_count.load(std::memory_order_relaxed),
        .maximum_active_region_lease_count =
            implementation_->maximum_active_region_lease_count.load(std::memory_order_relaxed),
        .invalidation_count = implementation_->invalidation_count.load(std::memory_order_relaxed),
        .completed_fence_value =
            implementation_->completed_fence_value.load(std::memory_order_acquire),
        .retained_device_bytes =
            implementation_->retained_device_bytes.load(std::memory_order_relaxed),
        .published = true,
        .source_buffer_released =
            implementation_->source_buffer_release_count.load(std::memory_order_relaxed) == 1U,
        .invalidated = implementation_->invalidated.load(std::memory_order_acquire),
    };
}

MetalResidentRawRegionLease
MetalResidentRawSource::develop_device_region(const GeometryPixelRect requested_core) const {
    return develop_device_region(requested_core, std::numeric_limits<std::uint64_t>::max());
}

MetalResidentRawRegionLease MetalResidentRawSource::develop_device_region(
    const GeometryPixelRect requested_core,
    const std::uint64_t source_resident_allowance_bytes
) const {
    @autoreleasepool {
        if (!valid()) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "Metal resident RAW source is invalidated"
            );
        }
        if (!rect_inside(requested_core, dimensions())) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                0,
                "Metal resident RAW region must be inside the oriented source"
            );
        }
        const std::size_t output_bytes = checked_region_bytes(requested_core);
        if (output_bytes > region_buffer_limit(implementation_->device)) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "Metal resident RAW region exceeds this device's buffer limit"
            );
        }
        std::size_t slot_index = 0U;
        try {
            slot_index =
                implementation_->acquire_output_slot(output_bytes, source_resident_allowance_bytes);
        } catch (...) {
            implementation_->invalidate();
            throw;
        }
        bool slot_held = true;
        const auto release_slot = [&]() noexcept {
            if (slot_held) {
                implementation_->release_output_slot(slot_index);
                slot_held = false;
            }
        };

        try {
            if (environment_enabled("SHADOW_TEST_METAL_RESIDENT_FAIL_REGION")) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "test-forced Metal resident RAW region failure"
                );
            }
            id<MTLCommandBuffer> command_buffer = [implementation_->queue commandBuffer];
            if (command_buffer == nil) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Metal resident RAW source could not create a command buffer"
                );
            }
            id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
            if (encoder == nil) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Metal resident RAW source could not create a reconstruction encoder"
                );
            }
            const ResidentRawDevelopmentParameters parameters = make_parameters(
                implementation_->descriptor,
                implementation_->transform,
                implementation_->development_plan,
                requested_core
            );
            const std::array<std::uint32_t, 2U> output_origin{
                requested_core.x,
                requested_core.y,
            };
            const NSUInteger thread_width = std::min<NSUInteger>(
                32U,
                std::max<NSUInteger>(1U, implementation_->pipeline.threadExecutionWidth)
            );
            const NSUInteger thread_height = std::max<NSUInteger>(
                1U,
                std::min<NSUInteger>(
                    8U,
                    implementation_->pipeline.maxTotalThreadsPerThreadgroup / thread_width
                )
            );
            [encoder setComputePipelineState:implementation_->pipeline];
            [encoder setBuffer:implementation_->resident_samples offset:0U atIndex:0U];
            [encoder setBuffer:implementation_->output_slots[slot_index].buffer
                        offset:0U
                       atIndex:1U];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
            [encoder setBytes:output_origin.data() length:sizeof(output_origin) atIndex:3U];
            [encoder dispatchThreads:MTLSizeMake(requested_core.width, requested_core.height, 1U)
                threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
            [encoder endEncoding];

            if (implementation_->dcp_encoding != nullptr) {
                id<MTLComputeCommandEncoder> dcp_encoder = [command_buffer computeCommandEncoder];
                std::string diagnostic;
                const std::uint64_t pixel_count =
                    static_cast<std::uint64_t>(requested_core.width) * requested_core.height;
                if (dcp_encoder == nil || pixel_count > std::numeric_limits<std::uint32_t>::max()
                    || !implementation_->dcp_encoding->encode(
                        dcp_encoder,
                        implementation_->output_slots[slot_index].buffer,
                        static_cast<std::uint32_t>(pixel_count),
                        diagnostic
                    )) {
                    if (dcp_encoder != nil) {
                        [dcp_encoder endEncoding];
                    }
                    throw DecodeError(
                        DecodeErrorCode::internal,
                        0,
                        diagnostic.empty()
                            ? "Metal resident RAW source could not encode DCP rendering"
                            : std::move(diagnostic)
                    );
                }
                [dcp_encoder endEncoding];
            }

            const std::uint64_t fence_value =
                implementation_->next_fence_value.fetch_add(1U, std::memory_order_relaxed);
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    detail::metal_raw_command_buffer_diagnostic(command_buffer)
                );
            }
            implementation_->region_dispatch_count.fetch_add(1U, std::memory_order_relaxed);
            store_completed_fence(implementation_->completed_fence_value, fence_value);
            if (!valid()) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Metal resident RAW source was invalidated by a concurrent request"
                );
            }

            slot_held = false;
            return MetalResidentRawRegionLease(
                std::make_unique<MetalResidentRawRegionLease::Impl>(
                    implementation_,
                    slot_index,
                    requested_core,
                    output_bytes,
                    fence_value
                )
            );
        } catch (...) {
            release_slot();
            implementation_->invalidate();
            throw;
        }
    }
}

DevelopedMetalResidentRawRegion
MetalResidentRawSource::develop_region(const GeometryPixelRect requested_core) const {
    auto lease = develop_device_region(requested_core);
    return lease.readback_compatibility();
}

bool metal_resident_raw_source_available() noexcept {
    return detail::metal_raw_resident_reconstruction_available();
}

ResidentRawSourceAttempt try_prepare_metal_resident_raw_source(
    PreparedRawFrameSource prepared,
    detail::PreparedSceneLinearRegionOptics optics
) {
    RawFrame& frame = prepared.frame_;
    PreparedRawFrameDevelopment& development = prepared.development_;
    if (!prepared.owns_region_optics(optics)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "prepared RAW source cannot publish with independently prepared region optics"
        );
    }
    if (development.requested_backend() == RawDevelopmentBackendMode::cpu) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "a CPU-prepared RAW source cannot enter the Metal resident transaction"
        );
    }
    const bool forced_metal = development.requested_backend() == RawDevelopmentBackendMode::metal;
    const auto fail = [&prepared, forced_metal](std::string diagnostic) {
        if (forced_metal) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                diagnostic.empty() ? "Metal resident RAW source is unavailable"
                                   : std::move(diagnostic)
            );
        }
        return ResidentRawSourceAttempt{
            .source = nullptr,
            .fallback_source = std::optional<PreparedRawFrameSource>(std::move(prepared)),
            .diagnostic = std::move(diagnostic),
        };
    };
    if (development.neural_raw_denoise().execution_requested()) {
        // Core ML is a whole-frame RAW-to-RAW transaction in v0. Hand the still-owned source back
        // to the normal materializer even for a forced Metal reconstruction request; that path
        // runs Core ML first and can still select Metal for conventional CFA denoise/demosaic.
        return ResidentRawSourceAttempt{
            .source = nullptr,
            .fallback_source = std::optional<PreparedRawFrameSource>(std::move(prepared)),
            .diagnostic =
                "neural RAW denoise requires the materialized source transaction",
        };
    }
    if (!frame.valid() || !frame.is_bayer_2x2()) {
        return fail("Metal resident RAW source requires a valid Bayer two-by-two RawFrame");
    }
    detail::NeuralRawDenoiseResult neural_denoised =
        detail::execute_prepared_neural_raw_denoise(
            std::move(frame),
            development.neural_raw_denoise()
        );
    frame = std::move(neural_denoised.frame);
    if (development.preview_max_edge().has_value()
        || development.reconstruction_dimensions() != frame.descriptor.active_dimensions) {
        return fail("Metal resident RAW source only accepts native-size development plans");
    }
    if (!optics.device_resident_eligible()) {
        return fail("prepared scene-linear optics cannot execute in the resident device path");
    }
    if (!detail::metal_raw_resident_reconstruction_available()) {
        return fail(detail::metal_raw_resident_reconstruction_diagnostic());
    }
    if (environment_enabled("SHADOW_TEST_METAL_RESIDENT_FAIL_PREPARE")) {
        return fail("test-forced Metal resident RAW preparation failure");
    }

    std::size_t sensor_bytes = 0U;
    try {
        sensor_bytes = checked_sensor_bytes(frame);
    } catch (const DecodeError& error) {
        return fail(error.what());
    }
    id<MTLDevice> device = detail::metal_raw_device();
    if (device == nil || sensor_bytes > static_cast<std::size_t>(device.maxBufferLength)) {
        return fail("RAW sensor plane exceeds this Metal device's resident buffer limit");
    }

    std::unique_ptr<detail::MetalDcpColorEncoding> dcp_encoding;
    std::size_t dcp_resource_bytes = 0U;
    const DcpColorTransform* camera_profile = development.camera_profile();
    if (camera_profile != nullptr && camera_profile->has_post_matrix_stages()) {
        const auto maximum_region_pixels = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max(),
            static_cast<std::uint64_t>(device.maxBufferLength) / (3U * sizeof(float))
        ));
        std::string diagnostic;
        dcp_encoding = detail::MetalDcpColorEncoding::prepare(
            *camera_profile,
            maximum_region_pixels,
            diagnostic
        );
        if (dcp_encoding == nullptr) {
            return fail(
                diagnostic.empty() ? "Metal resident RAW source could not prepare DCP rendering"
                                   : std::move(diagnostic)
            );
        }
        dcp_resource_bytes = dcp_encoding->resource_bytes();
    }

    std::size_t two_sensor_buffers = 0U;
    std::size_t preparation_working_set = 0U;
    if (!detail::checked_multiply(sensor_bytes, 2U, two_sensor_buffers)
        || !detail::checked_add(two_sensor_buffers, dcp_resource_bytes, preparation_working_set)) {
        return fail("Metal resident RAW preparation working set overflows");
    }
    const auto recommended = static_cast<std::size_t>(device.recommendedMaxWorkingSetSize);
    if (recommended > 0U && preparation_working_set > recommended / 3U) {
        return fail("RAW sensor residency exceeds Shadow's Metal working-set allowance");
    }

    std::optional<detail::MetalRawDenoiseEncoding> denoise_encoding;
    if (development.raw_denoise().applied()) {
        std::string diagnostic;
        denoise_encoding = detail::MetalRawDenoiseEncoding::prepare(
            frame,
            development.raw_denoise().mode,
            development.raw_denoise().iso_sensitivity,
            diagnostic
        );
        if (!denoise_encoding.has_value()) {
            return fail(
                diagnostic.empty() ? "Metal resident RAW source could not prepare CFA denoise"
                                   : std::move(diagnostic)
            );
        }
    }

    @autoreleasepool {
        detail::OwnedObjectiveCObject source_buffer([device
            newBufferWithBytes:frame.samples.data()
                        length:sensor_bytes
                       options:MTLResourceStorageModeShared]);
        if (!source_buffer) {
            return fail("Metal resident RAW source could not upload the sensor plane");
        }
        detail::OwnedObjectiveCObject resident_buffer([device
            newBufferWithLength:sensor_bytes
                        options:MTLResourceStorageModePrivate]);
        if (!resident_buffer) {
            return fail("Metal resident RAW source could not allocate the denoised CFA plane");
        }
        detail::OwnedObjectiveCObject queue([device newCommandQueue]);
        if (!queue) {
            return fail("Metal resident RAW source could not create its command queue");
        }
        id<MTLCommandBuffer> command_buffer =
            [static_cast<id<MTLCommandQueue>>(queue.get()) commandBuffer];
        if (command_buffer == nil) {
            return fail("Metal resident RAW source could not create its publication command");
        }
        if (denoise_encoding.has_value()) {
            std::string diagnostic;
            if (!denoise_encoding->encode(
                    command_buffer,
                    static_cast<id<MTLBuffer>>(source_buffer.get()),
                    static_cast<id<MTLBuffer>>(resident_buffer.get()),
                    diagnostic
                )) {
                return fail(
                    diagnostic.empty() ? "Metal resident RAW source could not encode CFA denoise"
                                       : std::move(diagnostic)
                );
            }
        } else {
            id<MTLBlitCommandEncoder> copy_encoder = [command_buffer blitCommandEncoder];
            if (copy_encoder == nil) {
                return fail("Metal resident RAW source could not create its source-copy encoder");
            }
            [copy_encoder copyFromBuffer:static_cast<id<MTLBuffer>>(source_buffer.get())
                            sourceOffset:0U
                                toBuffer:static_cast<id<MTLBuffer>>(resident_buffer.get())
                       destinationOffset:0U
                                    size:sensor_bytes];
            [copy_encoder endEncoding];
        }
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return fail(detail::metal_raw_command_buffer_diagnostic(command_buffer));
        }

        auto implementation = std::make_shared<MetalResidentRawSource::Impl>();
        implementation->descriptor = frame.descriptor;
        implementation->transform = development.linear_transform();
        implementation->development_plan = development.development_plan();
        implementation->raw_denoise_receipt = detail::finalize_raw_bayer_denoise_receipt(
            frame,
            development.raw_denoise(),
            development.raw_denoise().applied() ? RawBayerDenoiseBackend::metal
                                                : RawBayerDenoiseBackend::cpu
        );
        implementation->dcp_resource_bytes = dcp_resource_bytes;
        implementation->dcp_encoding = std::move(dcp_encoding);
        implementation->device = [device retain];
        implementation->queue = [static_cast<id<MTLCommandQueue>>(queue.get()) retain];
        implementation->pipeline = [detail::metal_raw_resident_reconstruction_pipeline() retain];
        implementation->resident_samples =
            [static_cast<id<MTLBuffer>>(resident_buffer.get()) retain];
        implementation->source_upload_bytes.store(
            static_cast<std::uint64_t>(sensor_bytes),
            std::memory_order_relaxed
        );
        implementation->denoise_dispatch_count.store(
            development.raw_denoise().applied() ? 1U : 0U,
            std::memory_order_relaxed
        );
        implementation->retained_device_bytes.store(
            static_cast<std::uint64_t>(sensor_bytes + dcp_resource_bytes),
            std::memory_order_relaxed
        );

        auto metal_source = std::unique_ptr<MetalResidentRawSource>(
            new MetalResidentRawSource(std::move(implementation))
        );
        std::vector<std::uint16_t>().swap(frame.samples);

        const RawDevelopmentPlanNegotiationStatus plan_negotiation_status =
            prepared.plan_negotiation_status_;
        RawPipelineReceipt pipeline = std::move(prepared.pipeline_);
        RawDevelopmentReceipt raw_receipt = finalize_raw_frame_development_receipt(
            development,
            metal_source->dimensions(),
            metal_source->demosaic_receipt(),
            RawDevelopmentBackend::metal,
            neural_denoised.receipt,
            metal_source->raw_denoise_receipt(),
            metal_source->dcp_applied() ? DcpColorExecutionBackend::metal
                                        : DcpColorExecutionBackend::cpu
        );
        raw_receipt.requested_plan = pipeline.requested_plan;
        raw_receipt.requested_plan_identity =
            raw_development_plan_identity(pipeline.requested_plan);
        raw_receipt.effective_plan = pipeline.effective_plan;
        raw_receipt.effective_plan_identity =
            raw_development_plan_identity(pipeline.effective_plan);
        raw_receipt.plan_negotiation_status = plan_negotiation_status;
        pipeline = finalize_raw_frame_pipeline_receipt(
            std::move(pipeline),
            RawDevelopmentBackend::metal,
            development.development_plan().highlight_recovery,
            detail::combined_raw_denoise_cache_identity(
                neural_denoised.receipt,
                metal_source->raw_denoise_receipt()
            )
        );

        return ResidentRawSourceAttempt{
            .source = std::unique_ptr<ResidentRawSource>(new ResidentRawSource(
                std::move(metal_source),
                std::move(development),
                std::move(optics),
                std::move(raw_receipt),
                std::move(pipeline)
            )),
            .fallback_source = std::nullopt,
            .diagnostic = {},
        };
    }
}

} // namespace shadow::image::raw_pipeline_detail
