// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"
#include "warm_edit_gpu_pipeline_context.hpp"
#include "warm_edit_gpu_render_plan.hpp"
#include "../edit/adjustment_execution_internal.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace shadow::image::detail {

namespace {

inline constexpr std::size_t warm_slot_count = 2U;
inline constexpr std::size_t maximum_warm_adjustment_operations = 256U;
inline constexpr std::size_t maximum_resident_curve_tables = 16U;
inline constexpr std::size_t maximum_resident_lut_tables = 4U;
inline constexpr std::size_t maximum_resident_perceptual_mixer_tables = 16U;
inline constexpr std::size_t maximum_resident_perceptual_range_tables = 16U;
inline constexpr std::size_t maximum_resident_selective_color_tables = 16U;

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

[[nodiscard]] bool force_test_failure() noexcept {
    const char* configured = std::getenv("SHADOW_TEST_WARM_METAL_FORCE_FAILURE");
    return configured != nullptr && std::string_view(configured) == "1";
}

struct WarmSlot final {
    id<MTLBuffer> adjusted = nil;
    id<MTLBuffer> denoised = nil;
    id<MTLBuffer> sharpen_log_luminance = nil;
    id<MTLBuffer> sharpen_horizontal = nil;
    id<MTLBuffer> perceptual_small = nil;
    id<MTLBuffer> perceptual_texture = nil;
    id<MTLBuffer> local_contrast_a = nil;
    id<MTLBuffer> local_contrast_b = nil;
    id<MTLBuffer> rgb8 = nil;
    id<MTLBuffer> before_operations = nil;
    id<MTLBuffer> after_operations = nil;
    id<MTLBuffer> status = nil;
    bool busy = false;
};

class RetainedMetalBuffer final {
public:
    RetainedMetalBuffer() noexcept = default;

    explicit RetainedMetalBuffer(id<MTLBuffer> value) noexcept
        : value_(value) {
        [value_ retain];
    }

    ~RetainedMetalBuffer() {
        [value_ release];
    }

    RetainedMetalBuffer(const RetainedMetalBuffer&) = delete;
    RetainedMetalBuffer& operator=(const RetainedMetalBuffer&) = delete;

    RetainedMetalBuffer(RetainedMetalBuffer&& other) noexcept
        : value_(std::exchange(other.value_, nil)) {}

    RetainedMetalBuffer& operator=(RetainedMetalBuffer&& other) noexcept {
        if (this != &other) {
            [value_ release];
            value_ = std::exchange(other.value_, nil);
        }
        return *this;
    }

    [[nodiscard]] id<MTLBuffer> get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nil; }

private:
    id<MTLBuffer> value_ = nil;
};

struct WarmProgramBuffers final {
    RetainedMetalBuffer curve;
    RetainedMetalBuffer lut;
    RetainedMetalBuffer perceptual_mixer;
    RetainedMetalBuffer perceptual_range;
    RetainedMetalBuffer selective_color;
};

struct WarmProgramBufferAttempt final {
    WarmProgramBuffers buffers;
    bool cancelled = false;
    std::string diagnostic;
};

struct SideBufferAttempt final {
    RetainedMetalBuffer buffer;
    bool cancelled = false;
    std::string diagnostic;
};

[[nodiscard]] std::uint64_t side_table_content_hash(
    const std::span<const std::byte> bytes
) noexcept {
    // A transient accelerator only: exact bytes below remain authoritative against collisions.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct ResidentSideTable final {
    std::vector<std::byte> content;
    std::uint64_t content_hash = 0U;
    id<MTLBuffer> buffer = nil;
    std::uint64_t last_use = 0U;

    ResidentSideTable(
        std::vector<std::byte> bytes,
        const std::uint64_t hash,
        id<MTLBuffer> owned_buffer,
        const std::uint64_t use
    ) noexcept
        : content(std::move(bytes)),
          content_hash(hash),
          buffer(owned_buffer),
          last_use(use) {}

    ~ResidentSideTable() {
        [buffer release];
    }

    ResidentSideTable(const ResidentSideTable&) = delete;
    ResidentSideTable& operator=(const ResidentSideTable&) = delete;

    ResidentSideTable(ResidentSideTable&& other) noexcept
        : content(std::move(other.content)),
          content_hash(other.content_hash),
          buffer(std::exchange(other.buffer, nil)),
          last_use(other.last_use) {}

    ResidentSideTable& operator=(ResidentSideTable&& other) noexcept {
        if (this != &other) {
            [buffer release];
            content = std::move(other.content);
            content_hash = other.content_hash;
            buffer = std::exchange(other.buffer, nil);
            last_use = other.last_use;
        }
        return *this;
    }

    [[nodiscard]] bool matches(
        const std::uint64_t hash,
        const std::span<const std::byte> bytes
    ) const noexcept {
        return content_hash == hash && content.size() == bytes.size()
            && (bytes.empty()
                || std::memcmp(content.data(), bytes.data(), bytes.size()) == 0);
    }
};

} // namespace

struct WarmEditGpuSession::Impl final {
    id<MTLBuffer> source = nil;
    // A valid non-null binding is required even when one side table is empty. One immutable
    // zero buffer safely serves both arguments without treating emptiness as a cache upload.
    id<MTLBuffer> empty_side_table = nil;
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
    std::vector<ResidentSideTable> curve_tables;
    std::vector<ResidentSideTable> lut_tables;
    std::vector<ResidentSideTable> perceptual_mixer_tables;
    std::vector<ResidentSideTable> perceptual_range_tables;
    std::vector<ResidentSideTable> selective_color_tables;
    std::uint64_t side_table_use_sequence = 0U;

    mutable std::mutex mutex;
    mutable std::condition_variable_any available_slot;
    mutable std::size_t next_slot = 0U;
    mutable std::uint64_t active_renders = 0U;
    mutable WarmEditPreviewGpuStats stats;

    ~Impl() {
        for (auto& slot : slots) {
            [slot.status release];
            [slot.after_operations release];
            [slot.before_operations release];
            [slot.rgb8 release];
            [slot.local_contrast_b release];
            [slot.local_contrast_a release];
            [slot.perceptual_texture release];
            [slot.perceptual_small release];
            [slot.sharpen_horizontal release];
            [slot.sharpen_log_luminance release];
            [slot.denoised release];
            [slot.adjusted release];
        }
        [empty_side_table release];
        [source release];
    }

    template <typename Element>
    [[nodiscard]] SideBufferAttempt acquire_side_buffer(
        const std::vector<Element>& elements,
        const std::stop_token cancellation
    ) {
        static_assert(
            std::is_same_v<Element, MetalCurveSegment>
                || std::is_same_v<Element, MetalLutEntry>
                || std::is_same_v<Element, MetalPerceptualMixerEntry>
                || std::is_same_v<Element, MetalPerceptualRange>
                || std::is_same_v<Element, MetalSelectiveColorEntry>
        );
        if (cancellation.stop_requested()) {
            return SideBufferAttempt{.cancelled = true};
        }
        if (elements.empty()) {
            return SideBufferAttempt{
                .buffer = RetainedMetalBuffer(empty_side_table),
            };
        }

        const std::span<const Element> values(elements);
        const std::span<const std::byte> bytes = std::as_bytes(values);
        const std::uint64_t content_hash = side_table_content_hash(bytes);
        auto& cache = [&]() -> std::vector<ResidentSideTable>& {
            if constexpr (std::is_same_v<Element, MetalCurveSegment>) {
                return curve_tables;
            } else if constexpr (std::is_same_v<Element, MetalLutEntry>) {
                return lut_tables;
            } else if constexpr (
                std::is_same_v<Element, MetalPerceptualMixerEntry>
            ) {
                return perceptual_mixer_tables;
            } else if constexpr (
                std::is_same_v<Element, MetalPerceptualRange>
            ) {
                return perceptual_range_tables;
            } else {
                return selective_color_tables;
            }
        }();
        constexpr std::size_t capacity = [] {
            if constexpr (std::is_same_v<Element, MetalCurveSegment>) {
                return maximum_resident_curve_tables;
            } else if constexpr (std::is_same_v<Element, MetalLutEntry>) {
                return maximum_resident_lut_tables;
            } else if constexpr (
                std::is_same_v<Element, MetalPerceptualMixerEntry>
            ) {
                return maximum_resident_perceptual_mixer_tables;
            } else if constexpr (
                std::is_same_v<Element, MetalPerceptualRange>
            ) {
                return maximum_resident_perceptual_range_tables;
            } else {
                return maximum_resident_selective_color_tables;
            }
        }();

        std::lock_guard lock(mutex);
        if (cancellation.stop_requested()) {
            return SideBufferAttempt{.cancelled = true};
        }
        ++side_table_use_sequence;
        for (auto& entry : cache) {
            // The byte comparison is authoritative. No hash-only identity can alias two
            // immutable color-resource tables into the same resident buffer.
            if (entry.matches(content_hash, bytes)) {
                entry.last_use = side_table_use_sequence;
                ++stats.resource_cache_hit_count;
                return SideBufferAttempt{
                    .buffer = RetainedMetalBuffer(entry.buffer),
                };
            }
        }

        const std::size_t maximum_buffer_bytes =
            static_cast<std::size_t>(metal_context().device().maxBufferLength);
        if (bytes.size() > maximum_buffer_bytes) {
            return SideBufferAttempt{
                .diagnostic = "warm-preview adjustment side table exceeds the Metal buffer limit",
            };
        }
        // Copy the immutable identity before allocating the Metal object. Cache publication is
        // a single step after both are complete; cancellation never exposes a partial entry.
        std::vector<std::byte> owned_bytes(bytes.begin(), bytes.end());
        id<MTLBuffer> uploaded = [metal_context().device()
            newBufferWithBytes:bytes.data()
            length:bytes.size()
            options:MTLResourceStorageModeShared];
        if (uploaded == nil) {
            return SideBufferAttempt{
                .diagnostic = "Metal could not upload an adjustment side table",
            };
        }
        if (cancellation.stop_requested()) {
            [uploaded release];
            return SideBufferAttempt{.cancelled = true};
        }

        if (cache.size() >= capacity) {
            const auto oldest = std::min_element(
                cache.begin(),
                cache.end(),
                [](const ResidentSideTable& left, const ResidentSideTable& right) {
                    return left.last_use < right.last_use;
                }
            );
            stats.resident_bytes -= static_cast<std::uint64_t>(oldest->content.size());
            cache.erase(oldest);
        }
        cache.emplace_back(
            std::move(owned_bytes),
            content_hash,
            uploaded,
            side_table_use_sequence
        );
        ++stats.gpu_buffer_allocation_count;
        stats.resident_bytes += static_cast<std::uint64_t>(bytes.size());
        if constexpr (std::is_same_v<Element, MetalCurveSegment>) {
            ++stats.curve_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, MetalLutEntry>) {
            ++stats.lut_resource_upload_count;
        } else if constexpr (
            std::is_same_v<Element, MetalPerceptualMixerEntry>
        ) {
            ++stats.perceptual_mixer_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, MetalPerceptualRange>) {
            ++stats.perceptual_range_resource_upload_count;
        } else {
            ++stats.selective_color_resource_upload_count;
        }
        return SideBufferAttempt{
            // Cache owns uploaded's original +1; the returned retain protects an in-flight
            // command if a concurrent render evicts this LRU entry.
            .buffer = RetainedMetalBuffer(cache.back().buffer),
        };
    }

    [[nodiscard]] WarmProgramBufferAttempt acquire_program_buffers(
        const PreparedMetalAdjustment& program,
        const std::stop_token cancellation
    ) {
        WarmProgramBufferAttempt result;
        auto curve = acquire_side_buffer(program.curve_segments, cancellation);
        if (curve.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!curve.buffer) {
            result.diagnostic = curve.diagnostic.empty()
                ? "session-resident Metal warm preview has no curve side table"
                : std::move(curve.diagnostic);
            return result;
        }
        result.buffers.curve = std::move(curve.buffer);

        auto lut = acquire_side_buffer(program.lut_entries, cancellation);
        if (lut.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!lut.buffer) {
            result.diagnostic = lut.diagnostic.empty()
                ? "session-resident Metal warm preview has no LUT side table"
                : std::move(lut.diagnostic);
            return result;
        }
        result.buffers.lut = std::move(lut.buffer);

        auto perceptual_mixer = acquire_side_buffer(
            program.perceptual_mixer_entries,
            cancellation
        );
        if (perceptual_mixer.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!perceptual_mixer.buffer) {
            result.diagnostic = perceptual_mixer.diagnostic.empty()
                ? "session-resident Metal warm preview has no perceptual mixer table"
                : std::move(perceptual_mixer.diagnostic);
            return result;
        }
        result.buffers.perceptual_mixer = std::move(perceptual_mixer.buffer);

        auto perceptual_range = acquire_side_buffer(
            program.perceptual_range_entries,
            cancellation
        );
        if (perceptual_range.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!perceptual_range.buffer) {
            result.diagnostic = perceptual_range.diagnostic.empty()
                ? "session-resident Metal warm preview has no perceptual range table"
                : std::move(perceptual_range.diagnostic);
            return result;
        }
        result.buffers.perceptual_range = std::move(perceptual_range.buffer);

        auto selective_color = acquire_side_buffer(
            program.selective_color_entries,
            cancellation
        );
        if (selective_color.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!selective_color.buffer) {
            result.diagnostic = selective_color.diagnostic.empty()
                ? "session-resident Metal warm preview has no Selective Color table"
                : std::move(selective_color.diagnostic);
            return result;
        }
        result.buffers.selective_color = std::move(selective_color.buffer);
        return result;
    }

    [[nodiscard]] std::optional<std::size_t> acquire_slot(
        const std::stop_token cancellation
    ) {
        std::unique_lock lock(mutex);
        const bool available = available_slot.wait(lock, cancellation, [this]() {
            return std::ranges::any_of(slots, [](const WarmSlot& slot) {
                return !slot.busy;
            });
        });
        if (!available || cancellation.stop_requested()) {
            return std::nullopt;
        }
        for (std::size_t offset = 0U; offset < slots.size(); ++offset) {
            const std::size_t index = (next_slot + offset) % slots.size();
            if (!slots[index].busy) {
                slots[index].busy = true;
                next_slot = (index + 1U) % slots.size();
                ++active_renders;
                ++stats.render_count;
                stats.peak_concurrent_renders =
                    std::max(stats.peak_concurrent_renders, active_renders);
                return std::optional<std::size_t>{index};
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

    [[nodiscard]] std::string ensure_denoise_resources(
        const std::size_t index
    ) {
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.denoised != nil && slot.after_operations != nil) {
            return {};
        }
        if (slot.denoised != nil || slot.after_operations != nil) {
            return "warm-preview denoise slot was only partially initialized";
        }
        std::size_t addition = 0U;
        if (!checked_add(adjusted_bytes, operation_buffer_bytes, addition)
            || addition > std::numeric_limits<std::size_t>::max()
                - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview denoise resource size overflowed";
        }
        const auto recommended = static_cast<std::size_t>(
            metal_context().device().recommendedMaxWorkingSetSize
        );
        const std::size_t allowance = recommended / 2U;
        if (recommended > 0U && (addition > allowance
                || static_cast<std::size_t>(stats.resident_bytes)
                    > allowance - addition)) {
            return "warm-preview denoise resources exceed half the recommended Metal working set";
        }
        id<MTLBuffer> denoised = [metal_context().device()
            newBufferWithLength:adjusted_bytes
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> after_operations = [metal_context().device()
            newBufferWithLength:operation_buffer_bytes
            options:MTLResourceStorageModeShared];
        if (denoised == nil || after_operations == nil) {
            [denoised release];
            [after_operations release];
            return "Metal could not allocate a resident warm-preview denoise slot";
        }
        slot.denoised = denoised;
        slot.after_operations = after_operations;
        stats.gpu_buffer_allocation_count += 2U;
        stats.resident_bytes += static_cast<std::uint64_t>(addition);
        return {};
    }

    [[nodiscard]] std::string ensure_sharpen_resources(
        const std::size_t index
    ) {
        const std::string detail_diagnostic = ensure_denoise_resources(index);
        if (!detail_diagnostic.empty()) {
            return detail_diagnostic;
        }
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.sharpen_log_luminance != nil && slot.sharpen_horizontal != nil) {
            return {};
        }
        if (slot.sharpen_log_luminance != nil || slot.sharpen_horizontal != nil) {
            return "warm-preview sharpen slot was only partially initialized";
        }
        // adjusted_bytes is the already-checked packed RGB allocation for this slot.
        const std::size_t scalar_bytes = adjusted_bytes / 3U;
        std::size_t addition = 0U;
        if (!checked_add(scalar_bytes, scalar_bytes, addition)
            || addition > std::numeric_limits<std::size_t>::max()
                - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview sharpen resource size overflowed";
        }
        const auto recommended = static_cast<std::size_t>(
            metal_context().device().recommendedMaxWorkingSetSize
        );
        const std::size_t allowance = recommended / 2U;
        if (recommended > 0U && (addition > allowance
                || static_cast<std::size_t>(stats.resident_bytes)
                    > allowance - addition)) {
            return "warm-preview sharpen resources exceed half the recommended Metal working set";
        }
        id<MTLBuffer> log_luminance = [metal_context().device()
            newBufferWithLength:scalar_bytes
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> horizontal = [metal_context().device()
            newBufferWithLength:scalar_bytes
            options:MTLResourceStorageModeShared];
        if (log_luminance == nil || horizontal == nil) {
            [log_luminance release];
            [horizontal release];
            return "Metal could not allocate a resident warm-preview sharpen slot";
        }
        slot.sharpen_log_luminance = log_luminance;
        slot.sharpen_horizontal = horizontal;
        stats.gpu_buffer_allocation_count += 2U;
        stats.resident_bytes += static_cast<std::uint64_t>(addition);
        return {};
    }

    [[nodiscard]] std::string ensure_clarity_resources(
        const std::size_t index
    ) {
        const std::string detail_diagnostic = ensure_sharpen_resources(index);
        if (!detail_diagnostic.empty()) {
            return detail_diagnostic;
        }
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.perceptual_small != nil) {
            return {};
        }
        const std::size_t scalar_bytes = adjusted_bytes / 3U;
        if (scalar_bytes > std::numeric_limits<std::size_t>::max()
                - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview clarity resource size overflowed";
        }
        const auto recommended = static_cast<std::size_t>(
            metal_context().device().recommendedMaxWorkingSetSize
        );
        const std::size_t allowance = recommended / 2U;
        if (recommended > 0U && (scalar_bytes > allowance
                || static_cast<std::size_t>(stats.resident_bytes)
                    > allowance - scalar_bytes)) {
            return "warm-preview clarity resources exceed half the recommended Metal working set";
        }
        id<MTLBuffer> small = [metal_context().device()
            newBufferWithLength:scalar_bytes
            options:MTLResourceStorageModeShared];
        if (small == nil) {
            return "Metal could not allocate a resident warm-preview clarity raster";
        }
        slot.perceptual_small = small;
        ++stats.gpu_buffer_allocation_count;
        stats.resident_bytes += static_cast<std::uint64_t>(scalar_bytes);
        return {};
    }

    [[nodiscard]] std::string ensure_texture_clarity_resources(
        const std::size_t index
    ) {
        const std::string detail_diagnostic = ensure_clarity_resources(index);
        if (!detail_diagnostic.empty()) {
            return detail_diagnostic;
        }
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.perceptual_texture != nil) {
            return {};
        }
        const std::size_t scalar_bytes = adjusted_bytes / 3U;
        if (scalar_bytes > std::numeric_limits<std::size_t>::max()
                - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview texture-clarity resource size overflowed";
        }
        const auto recommended = static_cast<std::size_t>(
            metal_context().device().recommendedMaxWorkingSetSize
        );
        const std::size_t allowance = recommended / 2U;
        if (recommended > 0U && (scalar_bytes > allowance
                || static_cast<std::size_t>(stats.resident_bytes)
                    > allowance - scalar_bytes)) {
            return "warm-preview texture-clarity resources exceed half the recommended Metal working set";
        }
        id<MTLBuffer> texture = [metal_context().device()
            newBufferWithLength:scalar_bytes options:MTLResourceStorageModeShared];
        if (texture == nil) {
            return "Metal could not allocate a resident warm-preview texture raster";
        }
        slot.perceptual_texture = texture;
        ++stats.gpu_buffer_allocation_count;
        stats.resident_bytes += static_cast<std::uint64_t>(scalar_bytes);
        return {};
    }

    [[nodiscard]] std::string ensure_local_contrast_resources(
        const std::size_t index
    ) {
        const std::string detail_diagnostic = ensure_clarity_resources(index);
        if (!detail_diagnostic.empty()) {
            return detail_diagnostic;
        }
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.perceptual_texture != nil && slot.local_contrast_a != nil
            && slot.local_contrast_b != nil) {
            return {};
        }
        const std::size_t scalar_bytes = adjusted_bytes / 3U;
        const std::size_t missing = (slot.perceptual_texture == nil ? 1U : 0U)
            + (slot.local_contrast_a == nil ? 1U : 0U)
            + (slot.local_contrast_b == nil ? 1U : 0U);
        std::size_t addition = 0U;
        if (!checked_multiply(scalar_bytes, missing, addition)
            || addition > std::numeric_limits<std::size_t>::max()
                - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview local-contrast resource size overflowed";
        }
        const auto recommended = static_cast<std::size_t>(
            metal_context().device().recommendedMaxWorkingSetSize
        );
        const std::size_t allowance = recommended / 2U;
        if (recommended > 0U && (addition > allowance
                || static_cast<std::size_t>(stats.resident_bytes)
                    > allowance - addition)) {
            return "warm-preview local-contrast resources exceed half the recommended Metal working set";
        }
        id<MTLBuffer> texture = slot.perceptual_texture == nil
            ? [metal_context().device() newBufferWithLength:scalar_bytes
                options:MTLResourceStorageModeShared]
            : nil;
        id<MTLBuffer> a = slot.local_contrast_a == nil
            ? [metal_context().device() newBufferWithLength:scalar_bytes
                options:MTLResourceStorageModeShared]
            : nil;
        id<MTLBuffer> b = slot.local_contrast_b == nil
            ? [metal_context().device() newBufferWithLength:scalar_bytes
                options:MTLResourceStorageModeShared]
            : nil;
        if ((slot.perceptual_texture == nil && texture == nil)
            || (slot.local_contrast_a == nil && a == nil)
            || (slot.local_contrast_b == nil && b == nil)) {
            [texture release];
            [a release];
            [b release];
            return "Metal could not allocate resident warm-preview local-contrast rasters";
        }
        if (slot.perceptual_texture == nil) {
            slot.perceptual_texture = texture;
        }
        if (slot.local_contrast_a == nil) {
            slot.local_contrast_a = a;
        }
        if (slot.local_contrast_b == nil) {
            slot.local_contrast_b = b;
        }
        stats.gpu_buffer_allocation_count += missing;
        stats.resident_bytes += static_cast<std::uint64_t>(addition);
        return {};
    }
};

WarmEditGpuSession::WarmEditGpuSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

WarmEditGpuSession::~WarmEditGpuSession() = default;

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const bool retain_linear_for_analysis,
    const std::stop_token cancellation
) const {
    const auto cancelled = [] {
        return RenderAttempt{
            .status = RenderStatus::cancelled,
            .output = std::nullopt,
            .diagnostic = {},
        };
    };
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    if (!impl_) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = "session-resident Metal warm preview is not initialized",
        };
    }
    if (force_test_failure()) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = "test-injected session-resident Metal warm-preview failure",
        };
    }

    const FloatRgbImage source_layout{
        .dimensions = impl_->dimensions,
        .row_stride_bytes = impl_->row_stride_bytes,
        .pixel_format = impl_->pixel_format,
        .transfer_function = impl_->transfer_function,
        .reference = impl_->reference,
        .working_space = impl_->working_space,
        .level_zero_to_raster_scale_x = impl_->level_zero_to_raster_scale_x,
        .level_zero_to_raster_scale_y = impl_->level_zero_to_raster_scale_y,
        // The immutable source was fully validated before upload. Warm parameter preparation
        // needs its layout/color metadata, not another full-raster finiteness scan.
        .samples = {},
    };
    std::string preparation_diagnostic;
    const auto prepare_program = [&source_layout, this, &preparation_diagnostic](
        const std::span<const AdjustmentNode> program_nodes,
        const EditExecutionPlan& candidate,
        const std::uint32_t input_row_floats,
        const std::uint32_t output_row_floats
    ) -> std::optional<PreparedMetalAdjustment> {
        PreparedMetalAdjustment result;
        if (candidate.segments.empty()) {
            result.invocation.width = impl_->dimensions.width;
            result.invocation.height = impl_->dimensions.height;
            result.invocation.step_count = 0U;
        } else {
            auto preparation = prepare_metal_adjustment(
                source_layout,
                program_nodes,
                candidate,
                AdjustmentExecutionContext{.full_dimensions = impl_->dimensions},
                true
            );
            if (!preparation.program.has_value()) {
                preparation_diagnostic = preparation.diagnostic.empty()
                    ? "session-resident Metal warm preview could not prepare the adjustment plan"
                    : std::move(preparation.diagnostic);
                return std::nullopt;
            }
            result = std::move(*preparation.program);
        }
        result.invocation.input_row_floats = input_row_floats;
        result.invocation.output_row_floats = output_row_floats;
        if (result.operations.size() > maximum_warm_adjustment_operations) {
            preparation_diagnostic =
                "session-resident Metal warm preview exceeds its 256-operation slot capacity";
            return std::nullopt;
        }
        return result;
    };

    const std::uint32_t source_row_floats =
        static_cast<std::uint32_t>(impl_->row_stride_bytes / sizeof(float));
    const std::uint32_t packed_row_floats = static_cast<std::uint32_t>(
        impl_->adjusted_row_stride_bytes / sizeof(float)
    );
    const WarmGpuRenderPlan render_plan = prepare_warm_gpu_render_plan(
        nodes,
        plan,
        impl_->dimensions,
        impl_->working_space,
        impl_->level_zero_to_raster_scale_x,
        impl_->level_zero_to_raster_scale_y
    );
    const auto* technical_detail_stage = std::get_if<WarmTechnicalDetailStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* texture_clarity_stage = std::get_if<WarmTextureClarityStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* local_contrast_stage = std::get_if<WarmLocalContrastStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* texture_stage = std::get_if<WarmTextureStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* clarity_stage = std::get_if<WarmClarityStage>(
        &render_plan.neighbourhood_stage
    );
    const auto* dehaze_defringe_stage = std::get_if<WarmDehazeDefringeStage>(
        &render_plan.neighbourhood_stage
    );
    const bool has_neighbourhood_stage = render_plan.has_neighbourhood_stage();
    PreparedMetalAdjustment before_program;
    std::optional<PreparedMetalAdjustment> final_program;
    if (technical_detail_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            technical_detail_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            nodes,
            technical_detail_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
        before_program = std::move(*prepared_before);
    } else if (texture_clarity_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            texture_clarity_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            texture_clarity_stage->post_nodes,
            texture_clarity_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = std::move(preparation_diagnostic)};
        }
        before_program = std::move(*prepared_before);
    } else if (local_contrast_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            local_contrast_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            local_contrast_stage->post_nodes,
            local_contrast_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = std::move(preparation_diagnostic)};
        }
        before_program = std::move(*prepared_before);
    } else if (texture_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            texture_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            texture_stage->post_nodes,
            texture_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
        before_program = std::move(*prepared_before);
    } else if (clarity_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            clarity_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            clarity_stage->post_nodes,
            clarity_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
        before_program = std::move(*prepared_before);
    } else if (dehaze_defringe_stage != nullptr) {
        auto prepared_before = prepare_program(
            nodes,
            dehaze_defringe_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            dehaze_defringe_stage->post_nodes,
            dehaze_defringe_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
        before_program = std::move(*prepared_before);
    } else {
        final_program = prepare_program(nodes, plan, source_row_floats, packed_row_floats);
        if (!final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
    }

    if (cancellation.stop_requested()) {
        return cancelled();
    }
    std::optional<WarmProgramBuffers> before_buffers;
    if (has_neighbourhood_stage) {
        auto attempt = impl_->acquire_program_buffers(before_program, cancellation);
        if (attempt.cancelled) {
            return cancelled();
        }
        if (!attempt.diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(attempt.diagnostic),
            };
        }
        before_buffers.emplace(std::move(attempt.buffers));
    }
    auto final_buffers_attempt = impl_->acquire_program_buffers(*final_program, cancellation);
    if (final_buffers_attempt.cancelled) {
        return cancelled();
    }
    if (!final_buffers_attempt.diagnostic.empty()) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = std::move(final_buffers_attempt.diagnostic),
        };
    }
    WarmProgramBuffers final_buffers = std::move(final_buffers_attempt.buffers);
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    const auto acquired_slot = impl_->acquire_slot(cancellation);
    if (!acquired_slot.has_value()) {
        return cancelled();
    }
    const std::size_t slot_index = *acquired_slot;
    bool completed = false;
    struct SlotRelease final {
        Impl& impl;
        std::size_t index;
        bool& completed;
        ~SlotRelease() { impl.release_slot(index, completed); }
    } release{*impl_, slot_index, completed};
    WarmSlot& slot = impl_->slots[slot_index];
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    if (technical_detail_stage != nullptr) {
        const std::string diagnostic = technical_detail_stage->sharpen.has_value()
            ? impl_->ensure_sharpen_resources(slot_index)
            : impl_->ensure_denoise_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    } else if (texture_clarity_stage != nullptr) {
        const std::string diagnostic = impl_->ensure_texture_clarity_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = diagnostic};
        }
    } else if (local_contrast_stage != nullptr) {
        const std::string diagnostic = impl_->ensure_local_contrast_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = diagnostic};
        }
    } else if (texture_stage != nullptr) {
        const std::string diagnostic = impl_->ensure_sharpen_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    } else if (clarity_stage != nullptr) {
        const std::string diagnostic = impl_->ensure_clarity_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    } else if (dehaze_defringe_stage != nullptr) {
        const std::string diagnostic = impl_->ensure_denoise_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    }

    @autoreleasepool {
        const auto upload_operations = [](id<MTLBuffer> destination,
                                          const PreparedMetalAdjustment& program) {
            const std::size_t bytes = program.operations.size()
                * sizeof(MetalAdjustmentOp);
            if (bytes > 0U) {
                std::memcpy([destination contents], program.operations.data(), bytes);
            }
        };
        if (has_neighbourhood_stage) {
            upload_operations(slot.before_operations, before_program);
            upload_operations(slot.after_operations, *final_program);
        } else {
            upload_operations(slot.before_operations, *final_program);
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
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = "Metal could not create a warm-preview compute command",
            };
        }
        const auto dispatch = [encoder, this](id<MTLComputePipelineState> pipeline) {
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
            [encoder dispatchThreads:MTLSizeMake(
                    impl_->dimensions.width,
                    impl_->dimensions.height,
                    1U
                )
                threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
        };
        const auto bind_adjustment = [&encoder, &slot](
            id<MTLBuffer> input,
            id<MTLBuffer> output,
            id<MTLBuffer> operations,
            const PreparedMetalAdjustment& program,
            const WarmProgramBuffers& buffers
        ) {
            [encoder setBuffer:input offset:0U atIndex:0U];
            [encoder setBuffer:output offset:0U atIndex:1U];
            [encoder setBuffer:operations offset:0U atIndex:3U];
            [encoder setBytes:&program.invocation
                       length:sizeof(program.invocation)
                      atIndex:4U];
            [encoder setBuffer:slot.status offset:0U atIndex:6U];
            [encoder setBuffer:buffers.curve.get() offset:0U atIndex:7U];
            [encoder setBuffer:buffers.lut.get() offset:0U atIndex:8U];
            [encoder setBuffer:buffers.perceptual_mixer.get() offset:0U atIndex:9U];
            [encoder setBuffer:buffers.perceptual_range.get() offset:0U atIndex:10U];
            [encoder setBuffer:buffers.selective_color.get() offset:0U atIndex:11U];
        };

        id<MTLBuffer> neighbourhood_output = impl_->source;
        if (technical_detail_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                impl_->source,
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());
            neighbourhood_output = slot.adjusted;

            if (technical_detail_stage->denoise.has_value()) {
                const auto& denoise = *technical_detail_stage->denoise;
                [encoder setComputePipelineState:context.denoise_pipeline()];
                [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
                [encoder setBuffer:slot.denoised offset:0U atIndex:1U];
                [encoder setBytes:&denoise length:sizeof(denoise) atIndex:2U];
                dispatch(context.denoise_pipeline());
                neighbourhood_output = slot.denoised;
                if (denoise.passes > 1U) {
                    [encoder setBuffer:slot.denoised offset:0U atIndex:0U];
                    [encoder setBuffer:slot.adjusted offset:0U atIndex:1U];
                    [encoder setBytes:&denoise length:sizeof(denoise) atIndex:2U];
                    dispatch(context.denoise_pipeline());
                    neighbourhood_output = slot.adjusted;
                }
            }

            if (technical_detail_stage->sharpen.has_value()) {
                const auto& sharpen = *technical_detail_stage->sharpen;
                [encoder setComputePipelineState:context.sharpen_log_pipeline()];
                [encoder setBuffer:neighbourhood_output offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
                [encoder setBytes:&sharpen length:sizeof(sharpen) atIndex:2U];
                dispatch(context.sharpen_log_pipeline());

                [encoder setComputePipelineState:context.sharpen_horizontal_pipeline()];
                [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBytes:&sharpen length:sizeof(sharpen) atIndex:2U];
                dispatch(context.sharpen_horizontal_pipeline());

                const id<MTLBuffer> sharpened_output = neighbourhood_output == slot.adjusted
                    ? slot.denoised
                    : slot.adjusted;
                [encoder setComputePipelineState:context.sharpen_apply_pipeline()];
                [encoder setBuffer:neighbourhood_output offset:0U atIndex:0U];
                [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
                [encoder setBuffer:sharpened_output offset:0U atIndex:2U];
                [encoder setBytes:&sharpen length:sizeof(sharpen) atIndex:3U];
                dispatch(context.sharpen_apply_pipeline());
                neighbourhood_output = sharpened_output;
            }
        } else if (texture_clarity_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(impl_->source, slot.adjusted, slot.before_operations,
                            before_program, *before_buffers);
            dispatch(context.adjustment_pipeline());

            const auto& texture = texture_clarity_stage->texture_gaussian;
            const auto& small = texture_clarity_stage->clarity_small_gaussian;
            const auto& large = texture_clarity_stage->clarity_large_gaussian;
            const auto& combined = texture_clarity_stage->parameters;
            [encoder setComputePipelineState:context.texture_lightness_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            [encoder setBytes:&final_program->invocation length:sizeof(final_program->invocation)
                      atIndex:3U];
            dispatch(context.texture_lightness_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());
            [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_texture offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            dispatch(context.scalar_vertical_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());
            [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_small offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            dispatch(context.scalar_vertical_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&large length:sizeof(large) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());
            [encoder setComputePipelineState:context.texture_clarity_apply_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_texture offset:0U atIndex:1U];
            [encoder setBuffer:slot.perceptual_small offset:0U atIndex:2U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:3U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:4U];
            [encoder setBytes:&combined length:sizeof(combined) atIndex:5U];
            [encoder setBytes:&final_program->invocation length:sizeof(final_program->invocation)
                      atIndex:6U];
            dispatch(context.texture_clarity_apply_pipeline());
            neighbourhood_output = slot.denoised;
        } else if (local_contrast_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                impl_->source,
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());

            const auto& small = local_contrast_stage->small_box;
            const auto& large = local_contrast_stage->large_box;
            const auto& small_coefficients = local_contrast_stage->small_coefficients;
            const auto& large_coefficients = local_contrast_stage->large_coefficients;
            const auto& local_contrast = local_contrast_stage->parameters;
            const WarmTextureParameters lightness{
                .width = impl_->dimensions.width,
                .height = impl_->dimensions.height,
            };
            const auto box_mean = [&encoder, &context, &dispatch](
                id<MTLBuffer> input,
                id<MTLBuffer> horizontal,
                id<MTLBuffer> output,
                const WarmBoxParameters& parameters
            ) {
                [encoder setComputePipelineState:context.box_horizontal_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:horizontal offset:0U atIndex:1U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
                dispatch(context.box_horizontal_pipeline());
                [encoder setComputePipelineState:context.box_vertical_pipeline()];
                [encoder setBuffer:horizontal offset:0U atIndex:0U];
                [encoder setBuffer:output offset:0U atIndex:1U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
                dispatch(context.box_vertical_pipeline());
            };
            const auto square = [&encoder, &context, &dispatch](
                id<MTLBuffer> input,
                id<MTLBuffer> output,
                const WarmBoxParameters& parameters
            ) {
                [encoder setComputePipelineState:context.scalar_square_pipeline()];
                [encoder setBuffer:input offset:0U atIndex:0U];
                [encoder setBuffer:output offset:0U atIndex:1U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
                dispatch(context.scalar_square_pipeline());
            };
            const auto coefficients = [&encoder, &context, &dispatch](
                id<MTLBuffer> guide,
                id<MTLBuffer> mean,
                id<MTLBuffer> variance,
                id<MTLBuffer> a,
                id<MTLBuffer> b,
                const WarmGuidedCoefficientsParameters& parameters
            ) {
                [encoder setComputePipelineState:context.guided_coefficients_pipeline()];
                [encoder setBuffer:guide offset:0U atIndex:0U];
                [encoder setBuffer:mean offset:0U atIndex:1U];
                [encoder setBuffer:variance offset:0U atIndex:2U];
                [encoder setBuffer:a offset:0U atIndex:3U];
                [encoder setBuffer:b offset:0U atIndex:4U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:5U];
                dispatch(context.guided_coefficients_pipeline());
            };
            const auto combine = [&encoder, &context, &dispatch](
                id<MTLBuffer> guide,
                id<MTLBuffer> a,
                id<MTLBuffer> b,
                id<MTLBuffer> output,
                const WarmBoxParameters& parameters
            ) {
                [encoder setComputePipelineState:context.guided_combine_pipeline()];
                [encoder setBuffer:guide offset:0U atIndex:0U];
                [encoder setBuffer:a offset:0U atIndex:1U];
                [encoder setBuffer:b offset:0U atIndex:2U];
                [encoder setBuffer:output offset:0U atIndex:3U];
                [encoder setBytes:&parameters length:sizeof(parameters) atIndex:4U];
                dispatch(context.guided_combine_pipeline());
            };

            // Scalar allocation layout:
            // guide=A, rolling horizontal=B, small output=C, large output=D,
            // coefficient scratch=E/F. Each phase overwrites only data whose
            // final use has passed, retaining both guided outputs for the
            // final broad-band residual.
            const id<MTLBuffer> guide = slot.sharpen_log_luminance;
            const id<MTLBuffer> horizontal = slot.sharpen_horizontal;
            const id<MTLBuffer> small_output = slot.perceptual_small;
            const id<MTLBuffer> large_output = slot.perceptual_texture;
            const id<MTLBuffer> scratch_a = slot.local_contrast_a;
            const id<MTLBuffer> scratch_b = slot.local_contrast_b;
            [encoder setComputePipelineState:context.texture_lightness_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:guide offset:0U atIndex:1U];
            [encoder setBytes:&lightness length:sizeof(lightness) atIndex:2U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation) atIndex:3U];
            dispatch(context.texture_lightness_pipeline());

            box_mean(guide, horizontal, small_output, small);
            square(guide, horizontal, small);
            box_mean(horizontal, scratch_a, large_output, small);
            coefficients(
                guide,
                small_output,
                large_output,
                scratch_a,
                scratch_b,
                small_coefficients
            );
            box_mean(scratch_a, horizontal, small_output, small);
            box_mean(scratch_b, horizontal, large_output, small);
            combine(guide, small_output, large_output, small_output, small);

            box_mean(guide, horizontal, large_output, large);
            square(guide, horizontal, large);
            box_mean(horizontal, scratch_a, scratch_b, large);
            coefficients(
                guide,
                large_output,
                scratch_b,
                scratch_a,
                horizontal,
                large_coefficients
            );
            box_mean(scratch_a, scratch_b, scratch_a, large);
            box_mean(horizontal, scratch_b, large_output, large);
            combine(guide, scratch_a, large_output, large_output, large);

            [encoder setComputePipelineState:context.local_contrast_apply_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:small_output offset:0U atIndex:1U];
            [encoder setBuffer:large_output offset:0U atIndex:2U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:3U];
            [encoder setBytes:&local_contrast length:sizeof(local_contrast) atIndex:4U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation) atIndex:5U];
            dispatch(context.local_contrast_apply_pipeline());
            neighbourhood_output = slot.denoised;
        } else if (texture_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                impl_->source,
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());

            const auto& texture = texture_stage->parameters;
            [encoder setComputePipelineState:context.texture_lightness_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:3U];
            dispatch(context.texture_lightness_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());

            [encoder setComputePipelineState:context.texture_apply_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:2U];
            [encoder setBytes:&texture length:sizeof(texture) atIndex:3U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:4U];
            dispatch(context.texture_apply_pipeline());
            neighbourhood_output = slot.denoised;
        } else if (clarity_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                impl_->source,
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());

            const auto& small = clarity_stage->small_gaussian;
            const auto& large = clarity_stage->large_gaussian;
            const auto& clarity = clarity_stage->parameters;
            [encoder setComputePipelineState:context.texture_lightness_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:3U];
            dispatch(context.texture_lightness_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());

            [encoder setComputePipelineState:context.scalar_vertical_pipeline()];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_small offset:0U atIndex:1U];
            [encoder setBytes:&small length:sizeof(small) atIndex:2U];
            dispatch(context.scalar_vertical_pipeline());

            [encoder setComputePipelineState:context.texture_horizontal_pipeline()];
            [encoder setBuffer:slot.sharpen_log_luminance offset:0U atIndex:0U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:1U];
            [encoder setBytes:&large length:sizeof(large) atIndex:2U];
            dispatch(context.texture_horizontal_pipeline());

            [encoder setComputePipelineState:context.clarity_apply_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.perceptual_small offset:0U atIndex:1U];
            [encoder setBuffer:slot.sharpen_horizontal offset:0U atIndex:2U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:3U];
            [encoder setBytes:&clarity length:sizeof(clarity) atIndex:4U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:5U];
            dispatch(context.clarity_apply_pipeline());
            neighbourhood_output = slot.denoised;
        } else if (dehaze_defringe_stage != nullptr) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                impl_->source,
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());

            const auto& technical_optics = dehaze_defringe_stage->parameters;
            [encoder setComputePipelineState:context.dehaze_defringe_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:1U];
            [encoder setBytes:&technical_optics length:sizeof(technical_optics) atIndex:2U];
            [encoder setBytes:&final_program->invocation
                       length:sizeof(final_program->invocation)
                      atIndex:3U];
            dispatch(context.dehaze_defringe_pipeline());
            neighbourhood_output = slot.denoised;
        }

        [encoder setComputePipelineState:context.display_pipeline()];
        id<MTLBuffer> final_input = neighbourhood_output;
        id<MTLBuffer> final_adjusted = has_neighbourhood_stage
            ? (final_input == slot.adjusted ? slot.denoised : slot.adjusted)
            : slot.adjusted;
        id<MTLBuffer> final_operations = has_neighbourhood_stage
            ? slot.after_operations
            : slot.before_operations;
        [encoder setBuffer:final_input offset:0U atIndex:0U];
        [encoder setBuffer:final_adjusted offset:0U atIndex:1U];
        [encoder setBuffer:slot.rgb8 offset:0U atIndex:2U];
        [encoder setBuffer:final_operations offset:0U atIndex:3U];
        [encoder setBytes:&final_program->invocation
                   length:sizeof(final_program->invocation)
                  atIndex:4U];
        [encoder setBytes:&display length:sizeof(display) atIndex:5U];
        [encoder setBuffer:slot.status offset:0U atIndex:6U];
        [encoder setBuffer:final_buffers.curve.get() offset:0U atIndex:7U];
        [encoder setBuffer:final_buffers.lut.get() offset:0U atIndex:8U];
        [encoder setBuffer:final_buffers.perceptual_mixer.get() offset:0U atIndex:9U];
        [encoder setBuffer:final_buffers.perceptual_range.get() offset:0U atIndex:10U];
        [encoder setBuffer:final_buffers.selective_color.get() offset:0U atIndex:11U];
        dispatch(context.display_pipeline());
        [encoder endEncoding];
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = command_buffer_diagnostic(command_buffer),
            };
        }
        if (status->flags != 0U) {
            std::string diagnostic =
                "session-resident Metal warm preview produced an invalid result";
            const auto append_node = [&diagnostic, status](
                const PreparedMetalAdjustment& program
            ) {
                if (status->earliest_step < program.operations.size()) {
                    diagnostic += " at source node " + std::to_string(
                        program.operations[status->earliest_step].source_node_index
                    );
                }
            };
            if (has_neighbourhood_stage) {
                append_node(before_program);
                append_node(*final_program);
            } else {
                append_node(*final_program);
            }
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
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
        if (cancellation.stop_requested()) {
            return cancelled();
        }
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
                [final_adjusted contents],
                impl_->adjusted_bytes
            );
            result.analyzed_linear = std::move(linear);
        }
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        completed = true;
        return RenderAttempt{
            .status = RenderStatus::completed,
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
    constexpr std::size_t maximum_shader_sample_index =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (sample_count > maximum_shader_sample_index
        || adjusted_sample_count > maximum_shader_sample_index) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic =
                "warm-preview raster exceeds the Metal kernel's uint32 sample address space",
        };
    }
    constexpr std::size_t operation_buffer_bytes =
        maximum_warm_adjustment_operations * sizeof(MetalAdjustmentOp);
    constexpr std::size_t empty_side_table_bytes = sizeof(MetalCurveSegment);
    const std::size_t maximum_buffer_bytes =
        static_cast<std::size_t>(context.device().maxBufferLength);
    if (source_bytes > maximum_buffer_bytes
        || adjusted_bytes > maximum_buffer_bytes
        || rgb8_bytes > maximum_buffer_bytes
        || operation_buffer_bytes > maximum_buffer_bytes
        || empty_side_table_bytes > maximum_buffer_bytes) {
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
        || !checked_add(source_bytes, slots_bytes, resident_bytes)
        || !checked_add(resident_bytes, empty_side_table_bytes, resident_bytes)) {
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
    impl->curve_tables.reserve(maximum_resident_curve_tables);
    impl->lut_tables.reserve(maximum_resident_lut_tables);
    impl->perceptual_mixer_tables.reserve(
        maximum_resident_perceptual_mixer_tables
    );
    impl->perceptual_range_tables.reserve(
        maximum_resident_perceptual_range_tables
    );
    impl->selective_color_tables.reserve(
        maximum_resident_selective_color_tables
    );
    impl->stats = WarmEditPreviewGpuStats{
        .resident = true,
        .source_upload_count = 1U,
        .gpu_buffer_allocation_count = 2U + warm_slot_count * 4U,
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
        const MetalCurveSegment empty_side_table{};
        impl->empty_side_table = [context.device()
            newBufferWithBytes:&empty_side_table
            length:sizeof(empty_side_table)
            options:MTLResourceStorageModeShared];
        if (impl->empty_side_table == nil) {
            return WarmEditGpuPreparation{
                .session = nullptr,
                .diagnostic = "Metal could not allocate the empty adjustment side table",
            };
        }
        for (auto& slot : impl->slots) {
            slot.adjusted = [context.device()
                newBufferWithLength:adjusted_bytes
                options:MTLResourceStorageModeShared];
            slot.rgb8 = [context.device()
                newBufferWithLength:rgb8_bytes
                options:MTLResourceStorageModeShared];
            slot.before_operations = [context.device()
                newBufferWithLength:operation_buffer_bytes
                options:MTLResourceStorageModeShared];
            slot.status = [context.device()
                newBufferWithLength:sizeof(WarmStatus)
                options:MTLResourceStorageModeShared];
            if (slot.adjusted == nil || slot.rgb8 == nil
                || slot.before_operations == nil || slot.status == nil) {
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
