#include "warm_edit_gpu_resident_resources.hpp"

#include "../edit/metal_adjustment_program.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"

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
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
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
inline constexpr std::size_t maximum_resident_brush_index_tables = 4U;
inline constexpr std::size_t maximum_resident_retouch_geometry_tables = 4U;
inline constexpr std::size_t maximum_resident_liquify_geometry_tables = 4U;

[[nodiscard]] std::size_t default_resident_allowance(id<MTLDevice> device) noexcept {
    const auto recommended = static_cast<std::size_t>(device.recommendedMaxWorkingSetSize);
    return recommended == 0U ? std::numeric_limits<std::size_t>::max() : recommended / 2U;
}

[[nodiscard]] bool
checked_multiply(const std::size_t left, const std::size_t right, std::size_t& result) noexcept {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool
checked_add(const std::size_t left, const std::size_t right, std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
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
    id<MTLBuffer> layer_before = nil;
    id<MTLBuffer> mask_coverage_linear = nil;
    id<MTLBuffer> mask_coverage_r8 = nil;
    id<MTLBuffer> retouch_statistics = nil;
    id<MTLBuffer> retouch_summary = nil;
    id<MTLBuffer> rgb8 = nil;
    id<MTLBuffer> before_operations = nil;
    id<MTLBuffer> after_operations = nil;
    id<MTLBuffer> status = nil;
    bool busy = false;
};

struct SideBufferAttempt final {
    RetainedMetalBuffer buffer;
    bool cancelled = false;
    std::string diagnostic;
};

[[nodiscard]] std::uint64_t
side_table_content_hash(const std::span<const std::byte> bytes) noexcept {
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
    ) noexcept :
        content(std::move(bytes)), content_hash(hash), buffer(owned_buffer), last_use(use) {}

    ~ResidentSideTable() {
        [buffer release];
    }

    ResidentSideTable(const ResidentSideTable&) = delete;
    ResidentSideTable& operator=(const ResidentSideTable&) = delete;

    ResidentSideTable(ResidentSideTable&& other) noexcept :
        content(std::move(other.content)), content_hash(other.content_hash),
        buffer(std::exchange(other.buffer, nil)), last_use(other.last_use) {}

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

    [[nodiscard]] bool
    matches(const std::uint64_t hash, const std::span<const std::byte> bytes) const noexcept {
        return content_hash == hash && content.size() == bytes.size()
               && (bytes.empty() || std::memcmp(content.data(), bytes.data(), bytes.size()) == 0);
    }
};

} // namespace

struct WarmGpuResidentResources::Impl final {
    id<MTLDevice> device = nil;
    id<MTLBuffer> source = nil;
    // One byte per output pixel, copied once from source-domain physical-white evidence. This
    // stays immutable beside the scene-linear source and is consumed only by selective tone.
    id<MTLBuffer> highlight_clipping = nil;
    // A valid non-null binding is required even when one side table is empty. One immutable
    // zero buffer safely serves every empty table without treating emptiness as a cache upload.
    id<MTLBuffer> empty_side_table = nil;
    std::array<WarmSlot, warm_slot_count> slots;
    WarmGpuResidentLayout layout;
    std::size_t operation_buffer_bytes = 0U;
    std::vector<ResidentSideTable> curve_tables;
    std::vector<ResidentSideTable> lut_tables;
    std::vector<ResidentSideTable> perceptual_mixer_tables;
    std::vector<ResidentSideTable> perceptual_range_tables;
    std::vector<ResidentSideTable> selective_color_tables;
    std::vector<ResidentSideTable> brush_index_tables;
    std::vector<ResidentSideTable> retouch_geometry_tables;
    std::vector<ResidentSideTable> liquify_geometry_tables;
    std::uint64_t side_table_use_sequence = 0U;
    std::size_t external_resident_bytes = 0U;
    std::size_t resident_allowance_bytes = std::numeric_limits<std::size_t>::max();

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
            [slot.mask_coverage_r8 release];
            [slot.mask_coverage_linear release];
            [slot.layer_before release];
            [slot.retouch_summary release];
            [slot.retouch_statistics release];
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
        [highlight_clipping release];
        [source release];
        brush_index_tables.clear();
        retouch_geometry_tables.clear();
        liquify_geometry_tables.clear();
        selective_color_tables.clear();
        perceptual_range_tables.clear();
        perceptual_mixer_tables.clear();
        lut_tables.clear();
        curve_tables.clear();
        [device release];
    }

    [[nodiscard]] bool fits_resident_addition(const std::size_t addition) const noexcept {
        std::size_t combined = 0U;
        return static_cast<std::uint64_t>(stats.resident_bytes)
                   <= std::numeric_limits<std::size_t>::max()
               && checked_add(
                   external_resident_bytes,
                   static_cast<std::size_t>(stats.resident_bytes),
                   combined
               )
               && checked_add(combined, addition, combined) && combined <= resident_allowance_bytes;
    }

    template <typename Element>
    [[nodiscard]] SideBufferAttempt
    acquire_side_buffer(const std::vector<Element>& elements, const std::stop_token cancellation) {
        static_assert(
            std::is_same_v<Element, MetalCurveSegment> || std::is_same_v<Element, MetalLutEntry>
            || std::is_same_v<Element, MetalPerceptualMixerEntry>
            || std::is_same_v<Element, MetalPerceptualRange>
            || std::is_same_v<Element, MetalSelectiveColorEntry>
            || std::is_same_v<Element, std::uint32_t> || std::is_same_v<Element, WarmRetouchWord>
            || std::is_same_v<Element, WarmPhotoLiquifyWord>
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
            } else if constexpr (std::is_same_v<Element, MetalPerceptualMixerEntry>) {
                return perceptual_mixer_tables;
            } else if constexpr (std::is_same_v<Element, MetalPerceptualRange>) {
                return perceptual_range_tables;
            } else if constexpr (std::is_same_v<Element, std::uint32_t>) {
                return brush_index_tables;
            } else if constexpr (std::is_same_v<Element, WarmRetouchWord>) {
                return retouch_geometry_tables;
            } else if constexpr (std::is_same_v<Element, WarmPhotoLiquifyWord>) {
                return liquify_geometry_tables;
            } else {
                return selective_color_tables;
            }
        }();
        constexpr std::size_t capacity = [] {
            if constexpr (std::is_same_v<Element, MetalCurveSegment>) {
                return maximum_resident_curve_tables;
            } else if constexpr (std::is_same_v<Element, MetalLutEntry>) {
                return maximum_resident_lut_tables;
            } else if constexpr (std::is_same_v<Element, MetalPerceptualMixerEntry>) {
                return maximum_resident_perceptual_mixer_tables;
            } else if constexpr (std::is_same_v<Element, MetalPerceptualRange>) {
                return maximum_resident_perceptual_range_tables;
            } else if constexpr (std::is_same_v<Element, std::uint32_t>) {
                return maximum_resident_brush_index_tables;
            } else if constexpr (std::is_same_v<Element, WarmRetouchWord>) {
                return maximum_resident_retouch_geometry_tables;
            } else if constexpr (std::is_same_v<Element, WarmPhotoLiquifyWord>) {
                return maximum_resident_liquify_geometry_tables;
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

        const std::size_t maximum_buffer_bytes = static_cast<std::size_t>(device.maxBufferLength);
        if (bytes.size() > maximum_buffer_bytes) {
            return SideBufferAttempt{
                .diagnostic = "warm-preview resident side table exceeds the Metal buffer limit",
            };
        }
        const auto evict_oldest = [&]() {
            const auto oldest = std::min_element(
                cache.begin(),
                cache.end(),
                [](const ResidentSideTable& left, const ResidentSideTable& right) {
                    return left.last_use < right.last_use;
                }
            );
            stats.resident_bytes -= static_cast<std::uint64_t>(oldest->content.size());
            cache.erase(oldest);
        };
        if (cache.size() >= capacity) {
            evict_oldest();
        }
        while (!fits_resident_addition(bytes.size()) && !cache.empty()) {
            evict_oldest();
        }
        if (!fits_resident_addition(bytes.size())) {
            return SideBufferAttempt{
                .diagnostic = "warm-preview resident side table exceeds the combined Metal budget",
            };
        }
        // Copy the immutable identity before allocating the Metal object. Cache publication is
        // a single step after both are complete; cancellation never exposes a partial entry.
        std::vector<std::byte> owned_bytes(bytes.begin(), bytes.end());
        id<MTLBuffer> uploaded = [device newBufferWithBytes:bytes.data()
                                                     length:bytes.size()
                                                    options:MTLResourceStorageModeShared];
        if (uploaded == nil) {
            return SideBufferAttempt{
                .diagnostic = "Metal could not upload a warm-preview resident side table",
            };
        }
        if (cancellation.stop_requested()) {
            [uploaded release];
            return SideBufferAttempt{.cancelled = true};
        }

        cache.emplace_back(std::move(owned_bytes), content_hash, uploaded, side_table_use_sequence);
        ++stats.gpu_buffer_allocation_count;
        stats.resident_bytes += static_cast<std::uint64_t>(bytes.size());
        if constexpr (std::is_same_v<Element, MetalCurveSegment>) {
            ++stats.curve_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, MetalLutEntry>) {
            ++stats.lut_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, MetalPerceptualMixerEntry>) {
            ++stats.perceptual_mixer_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, MetalPerceptualRange>) {
            ++stats.perceptual_range_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, std::uint32_t>) {
            ++stats.brush_index_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, WarmRetouchWord>) {
            ++stats.retouch_geometry_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, WarmPhotoLiquifyWord>) {
            // The generic allocation/cache-hit counters are the public
            // observability contract for this structural side table.
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

        auto perceptual_mixer = acquire_side_buffer(program.perceptual_mixer_entries, cancellation);
        if (perceptual_mixer.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!perceptual_mixer.buffer) {
            result.diagnostic =
                perceptual_mixer.diagnostic.empty()
                    ? "session-resident Metal warm preview has no perceptual mixer table"
                    : std::move(perceptual_mixer.diagnostic);
            return result;
        }
        result.buffers.perceptual_mixer = std::move(perceptual_mixer.buffer);

        auto perceptual_range = acquire_side_buffer(program.perceptual_range_entries, cancellation);
        if (perceptual_range.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!perceptual_range.buffer) {
            result.diagnostic =
                perceptual_range.diagnostic.empty()
                    ? "session-resident Metal warm preview has no perceptual range table"
                    : std::move(perceptual_range.diagnostic);
            return result;
        }
        result.buffers.perceptual_range = std::move(perceptual_range.buffer);

        auto selective_color = acquire_side_buffer(program.selective_color_entries, cancellation);
        if (selective_color.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!selective_color.buffer) {
            result.diagnostic =
                selective_color.diagnostic.empty()
                    ? "session-resident Metal warm preview has no Selective Color table"
                    : std::move(selective_color.diagnostic);
            return result;
        }
        result.buffers.selective_color = std::move(selective_color.buffer);
        return result;
    }

    [[nodiscard]] WarmBrushBufferAttempt acquire_brush_index_buffer(
        const std::vector<std::uint32_t>& words,
        const std::stop_token cancellation
    ) {
        auto attempt = acquire_side_buffer(words, cancellation);
        return WarmBrushBufferAttempt{
            .buffer = std::move(attempt.buffer),
            .cancelled = attempt.cancelled,
            .diagnostic = std::move(attempt.diagnostic),
        };
    }

    [[nodiscard]] WarmRetouchBufferAttempt acquire_retouch_geometry_buffer(
        const std::vector<WarmRetouchWord>& words,
        const std::stop_token cancellation
    ) {
        auto attempt = acquire_side_buffer(words, cancellation);
        return WarmRetouchBufferAttempt{
            .buffer = std::move(attempt.buffer),
            .cancelled = attempt.cancelled,
            .diagnostic = std::move(attempt.diagnostic),
        };
    }

    [[nodiscard]] WarmLiquifyBufferAttempt acquire_liquify_geometry_buffer(
        const std::vector<WarmPhotoLiquifyWord>& words,
        const std::stop_token cancellation
    ) {
        auto attempt = acquire_side_buffer(words, cancellation);
        return WarmLiquifyBufferAttempt{
            .buffer = std::move(attempt.buffer),
            .cancelled = attempt.cancelled,
            .diagnostic = std::move(attempt.diagnostic),
        };
    }

    [[nodiscard]] std::optional<std::size_t> acquire_slot(const std::stop_token cancellation) {
        std::unique_lock lock(mutex);
        const bool available = available_slot.wait(lock, cancellation, [this]() {
            return std::ranges::any_of(slots, [](const WarmSlot& slot) { return !slot.busy; });
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

    [[nodiscard]] std::string ensure_denoise_resources(const std::size_t index) {
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.denoised != nil && slot.after_operations != nil) {
            return {};
        }
        if (slot.denoised != nil || slot.after_operations != nil) {
            return "warm-preview denoise slot was only partially initialized";
        }
        std::size_t addition = 0U;
        if (!checked_add(layout.adjusted_bytes, operation_buffer_bytes, addition)
            || addition > std::numeric_limits<std::size_t>::max()
                              - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview denoise resource size overflowed";
        }
        if (!fits_resident_addition(addition)) {
            return "warm-preview denoise resources exceed the combined Metal budget";
        }
        id<MTLBuffer> denoised = [device newBufferWithLength:layout.adjusted_bytes
                                                     options:MTLResourceStorageModeShared];
        id<MTLBuffer> after_operations = [device newBufferWithLength:operation_buffer_bytes
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

    [[nodiscard]] std::string ensure_sharpen_resources(const std::size_t index) {
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
        // layout.adjusted_bytes is the already-checked packed RGB allocation for this slot.
        const std::size_t scalar_bytes = layout.adjusted_bytes / 3U;
        std::size_t addition = 0U;
        if (!checked_add(scalar_bytes, scalar_bytes, addition)
            || addition > std::numeric_limits<std::size_t>::max()
                              - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview sharpen resource size overflowed";
        }
        if (!fits_resident_addition(addition)) {
            return "warm-preview sharpen resources exceed the combined Metal budget";
        }
        id<MTLBuffer> log_luminance = [device newBufferWithLength:scalar_bytes
                                                          options:MTLResourceStorageModeShared];
        id<MTLBuffer> horizontal = [device newBufferWithLength:scalar_bytes
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

    [[nodiscard]] std::string ensure_clarity_resources(const std::size_t index) {
        const std::string detail_diagnostic = ensure_sharpen_resources(index);
        if (!detail_diagnostic.empty()) {
            return detail_diagnostic;
        }
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.perceptual_small != nil) {
            return {};
        }
        const std::size_t scalar_bytes = layout.adjusted_bytes / 3U;
        if (scalar_bytes > std::numeric_limits<std::size_t>::max()
                               - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview clarity resource size overflowed";
        }
        if (!fits_resident_addition(scalar_bytes)) {
            return "warm-preview clarity resources exceed the combined Metal budget";
        }
        id<MTLBuffer> small = [device newBufferWithLength:scalar_bytes
                                                  options:MTLResourceStorageModeShared];
        if (small == nil) {
            return "Metal could not allocate a resident warm-preview clarity raster";
        }
        slot.perceptual_small = small;
        ++stats.gpu_buffer_allocation_count;
        stats.resident_bytes += static_cast<std::uint64_t>(scalar_bytes);
        return {};
    }

    [[nodiscard]] std::string ensure_texture_clarity_resources(const std::size_t index) {
        const std::string detail_diagnostic = ensure_clarity_resources(index);
        if (!detail_diagnostic.empty()) {
            return detail_diagnostic;
        }
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.perceptual_texture != nil) {
            return {};
        }
        const std::size_t scalar_bytes = layout.adjusted_bytes / 3U;
        if (scalar_bytes > std::numeric_limits<std::size_t>::max()
                               - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview texture-clarity resource size overflowed";
        }
        if (!fits_resident_addition(scalar_bytes)) {
            return "warm-preview texture-clarity resources exceed the combined Metal budget";
        }
        id<MTLBuffer> texture = [device newBufferWithLength:scalar_bytes
                                                    options:MTLResourceStorageModeShared];
        if (texture == nil) {
            return "Metal could not allocate a resident warm-preview texture raster";
        }
        slot.perceptual_texture = texture;
        ++stats.gpu_buffer_allocation_count;
        stats.resident_bytes += static_cast<std::uint64_t>(scalar_bytes);
        return {};
    }

    [[nodiscard]] std::string ensure_local_contrast_resources(const std::size_t index) {
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
        const std::size_t scalar_bytes = layout.adjusted_bytes / 3U;
        const std::size_t missing = (slot.perceptual_texture == nil ? 1U : 0U)
                                    + (slot.local_contrast_a == nil ? 1U : 0U)
                                    + (slot.local_contrast_b == nil ? 1U : 0U);
        std::size_t addition = 0U;
        if (!checked_multiply(scalar_bytes, missing, addition)
            || addition > std::numeric_limits<std::size_t>::max()
                              - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview local-contrast resource size overflowed";
        }
        if (!fits_resident_addition(addition)) {
            return "warm-preview local-contrast resources exceed the combined Metal budget";
        }
        id<MTLBuffer> texture = slot.perceptual_texture == nil
                                    ? [device newBufferWithLength:scalar_bytes
                                                          options:MTLResourceStorageModeShared]
                                    : nil;
        id<MTLBuffer> a = slot.local_contrast_a == nil
                              ? [device newBufferWithLength:scalar_bytes
                                                    options:MTLResourceStorageModeShared]
                              : nil;
        id<MTLBuffer> b = slot.local_contrast_b == nil
                              ? [device newBufferWithLength:scalar_bytes
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

    [[nodiscard]] std::string ensure_layer_resources(const std::size_t index) {
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.layer_before != nil) {
            return {};
        }
        const std::size_t bytes = layout.adjusted_bytes;
        if (bytes == 0U
            || bytes > std::numeric_limits<std::size_t>::max()
                           - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview layer snapshot size overflowed";
        }
        if (!fits_resident_addition(bytes)) {
            return "warm-preview layer snapshot exceeds the combined Metal budget";
        }
        id<MTLBuffer> buffer = [device newBufferWithLength:bytes
                                                   options:MTLResourceStorageModeShared];
        if (buffer == nil) {
            return "Metal could not allocate the resident warm-preview layer snapshot";
        }
        slot.layer_before = buffer;
        ++stats.gpu_buffer_allocation_count;
        stats.resident_bytes += static_cast<std::uint64_t>(bytes);
        return {};
    }

    [[nodiscard]] std::string ensure_mask_coverage_resources(const std::size_t index) {
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.mask_coverage_linear != nil && slot.mask_coverage_r8 != nil) {
            return {};
        }
        if (slot.mask_coverage_linear != nil || slot.mask_coverage_r8 != nil) {
            return "warm-preview mask-coverage slot was only partially initialized";
        }
        const std::size_t linear_bytes = layout.adjusted_bytes / 3U;
        const std::size_t r8_bytes = layout.rgb8_bytes / 3U;
        std::size_t addition = 0U;
        if (linear_bytes == 0U || r8_bytes == 0U || !checked_add(linear_bytes, r8_bytes, addition)
            || addition > std::numeric_limits<std::size_t>::max()
                              - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview mask-coverage resource size overflowed";
        }
        if (!fits_resident_addition(addition)) {
            return "warm-preview mask-coverage resources exceed the combined Metal budget";
        }
        id<MTLBuffer> linear = [device newBufferWithLength:linear_bytes
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> r8 = [device newBufferWithLength:r8_bytes
                                               options:MTLResourceStorageModeShared];
        if (linear == nil || r8 == nil) {
            [linear release];
            [r8 release];
            return "Metal could not allocate resident warm-preview mask coverage";
        }
        slot.mask_coverage_linear = linear;
        slot.mask_coverage_r8 = r8;
        stats.gpu_buffer_allocation_count += 2U;
        stats.resident_bytes += static_cast<std::uint64_t>(addition);
        return {};
    }

    [[nodiscard]] std::string ensure_retouch_resources(const std::size_t index) {
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.retouch_statistics != nil && slot.retouch_summary != nil) {
            return {};
        }
        constexpr std::size_t statistics_threads = 256U;
        const std::size_t group_count =
            (layout.adjusted_sample_count / 3U + statistics_threads - 1U) / statistics_threads;
        std::size_t statistics_bytes = 0U;
        if (!checked_multiply(group_count, sizeof(WarmRetouchStatisticsPartial), statistics_bytes)
            || statistics_bytes == 0U) {
            return "warm-preview retouch statistics size overflowed";
        }
        const std::size_t summary_bytes = sizeof(WarmRetouchSummary);
        std::size_t addition = 0U;
        if (!checked_add(statistics_bytes, summary_bytes, addition)
            || addition > std::numeric_limits<std::size_t>::max()
                              - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview retouch resource size overflowed";
        }
        if (!fits_resident_addition(addition)) {
            return "warm-preview retouch resources exceed the combined Metal budget";
        }
        id<MTLBuffer> statistics = slot.retouch_statistics == nil
                                       ? [device newBufferWithLength:statistics_bytes
                                                             options:MTLResourceStorageModeShared]
                                       : nil;
        id<MTLBuffer> summary = slot.retouch_summary == nil
                                    ? [device newBufferWithLength:summary_bytes
                                                          options:MTLResourceStorageModeShared]
                                    : nil;
        if ((slot.retouch_statistics == nil && statistics == nil)
            || (slot.retouch_summary == nil && summary == nil)) {
            [statistics release];
            [summary release];
            return "Metal could not allocate resident warm-preview retouch statistics";
        }
        std::size_t allocations = 0U;
        if (slot.retouch_statistics == nil) {
            slot.retouch_statistics = statistics;
            ++allocations;
        }
        if (slot.retouch_summary == nil) {
            slot.retouch_summary = summary;
            ++allocations;
        }
        stats.gpu_buffer_allocation_count += allocations;
        stats.resident_bytes += static_cast<std::uint64_t>(addition);
        return {};
    }
};

RetainedMetalBuffer::RetainedMetalBuffer(id<MTLBuffer> value) noexcept : value_(value) {
    [value_ retain];
}

RetainedMetalBuffer::~RetainedMetalBuffer() {
    [value_ release];
}

RetainedMetalBuffer::RetainedMetalBuffer(RetainedMetalBuffer&& other) noexcept :
    value_(std::exchange(other.value_, nil)) {}

RetainedMetalBuffer& RetainedMetalBuffer::operator=(RetainedMetalBuffer&& other) noexcept {
    if (this != &other) {
        [value_ release];
        value_ = std::exchange(other.value_, nil);
    }
    return *this;
}

id<MTLBuffer> RetainedMetalBuffer::get() const noexcept {
    return value_;
}

RetainedMetalBuffer::operator bool() const noexcept {
    return value_ != nil;
}

WarmGpuResidentResources::WarmGpuResidentResources(std::unique_ptr<Impl> impl) :
    impl_(std::move(impl)) {}

WarmGpuResidentResources::~WarmGpuResidentResources() = default;

const WarmGpuResidentLayout& WarmGpuResidentResources::layout() const noexcept {
    return impl_->layout;
}

id<MTLBuffer> WarmGpuResidentResources::source_buffer() const noexcept {
    return impl_->source;
}

id<MTLBuffer> WarmGpuResidentResources::highlight_clipping_buffer() const noexcept {
    return impl_->highlight_clipping;
}

std::size_t WarmGpuResidentResources::operation_capacity() const noexcept {
    return maximum_warm_adjustment_operations;
}

WarmProgramBufferAttempt WarmGpuResidentResources::acquire_program_buffers(
    const PreparedMetalAdjustment& program,
    const std::stop_token cancellation
) {
    return impl_->acquire_program_buffers(program, cancellation);
}

WarmBrushBufferAttempt WarmGpuResidentResources::acquire_brush_index_buffer(
    const std::vector<std::uint32_t>& words,
    const std::stop_token cancellation
) {
    return impl_->acquire_brush_index_buffer(words, cancellation);
}

WarmRetouchBufferAttempt WarmGpuResidentResources::acquire_retouch_geometry_buffer(
    const std::vector<WarmRetouchWord>& words,
    const std::stop_token cancellation
) {
    return impl_->acquire_retouch_geometry_buffer(words, cancellation);
}

WarmLiquifyBufferAttempt WarmGpuResidentResources::acquire_liquify_geometry_buffer(
    const std::vector<WarmPhotoLiquifyWord>& words,
    const std::stop_token cancellation
) {
    return impl_->acquire_liquify_geometry_buffer(words, cancellation);
}

std::optional<WarmGpuSlotLease>
WarmGpuResidentResources::acquire_slot(const std::stop_token cancellation) {
    const auto index = impl_->acquire_slot(cancellation);
    if (!index.has_value()) {
        return std::nullopt;
    }
    WarmGpuSlotLease lease(*this, *index);
    return std::optional<WarmGpuSlotLease>(std::move(lease));
}

WarmEditPreviewGpuStats WarmGpuResidentResources::stats_snapshot() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

void WarmGpuResidentResources::record_presentation_surface_request() noexcept {
    std::lock_guard lock(impl_->mutex);
    ++impl_->stats.presentation_surface_request_count;
}

void WarmGpuResidentResources::record_presentation_surface_publish() noexcept {
    std::lock_guard lock(impl_->mutex);
    ++impl_->stats.presentation_surface_publish_count;
}

void WarmGpuResidentResources::record_presentation_surface_fallback() noexcept {
    std::lock_guard lock(impl_->mutex);
    ++impl_->stats.presentation_surface_fallback_count;
}

WarmGpuSlotBuffers WarmGpuResidentResources::slot_buffers(const std::size_t index) const noexcept {
    const WarmSlot& slot = impl_->slots[index];
    return WarmGpuSlotBuffers{
        .adjusted = slot.adjusted,
        .denoised = slot.denoised,
        .sharpen_log_luminance = slot.sharpen_log_luminance,
        .sharpen_horizontal = slot.sharpen_horizontal,
        .perceptual_small = slot.perceptual_small,
        .perceptual_texture = slot.perceptual_texture,
        .local_contrast_a = slot.local_contrast_a,
        .local_contrast_b = slot.local_contrast_b,
        .layer_before = slot.layer_before,
        .mask_coverage_linear = slot.mask_coverage_linear,
        .mask_coverage_r8 = slot.mask_coverage_r8,
        .retouch_statistics = slot.retouch_statistics,
        .retouch_summary = slot.retouch_summary,
        .rgb8 = slot.rgb8,
        .before_operations = slot.before_operations,
        .after_operations = slot.after_operations,
        .status = slot.status,
    };
}

std::string WarmGpuResidentResources::ensure_denoise_resources(const std::size_t index) {
    return impl_->ensure_denoise_resources(index);
}

std::string WarmGpuResidentResources::ensure_sharpen_resources(const std::size_t index) {
    return impl_->ensure_sharpen_resources(index);
}

std::string WarmGpuResidentResources::ensure_clarity_resources(const std::size_t index) {
    return impl_->ensure_clarity_resources(index);
}

std::string WarmGpuResidentResources::ensure_texture_clarity_resources(const std::size_t index) {
    return impl_->ensure_texture_clarity_resources(index);
}

std::string WarmGpuResidentResources::ensure_local_contrast_resources(const std::size_t index) {
    return impl_->ensure_local_contrast_resources(index);
}

std::string WarmGpuResidentResources::ensure_layer_resources(const std::size_t index) {
    return impl_->ensure_layer_resources(index);
}

std::string WarmGpuResidentResources::ensure_mask_coverage_resources(const std::size_t index) {
    return impl_->ensure_mask_coverage_resources(index);
}

std::string WarmGpuResidentResources::ensure_retouch_resources(const std::size_t index) {
    return impl_->ensure_retouch_resources(index);
}

void WarmGpuResidentResources::release_slot(
    const std::size_t index,
    const bool completed
) noexcept {
    impl_->release_slot(index, completed);
}

WarmGpuSlotLease::WarmGpuSlotLease(
    WarmGpuResidentResources& owner,
    const std::size_t index
) noexcept : owner_(&owner), index_(index) {}

WarmGpuSlotLease::WarmGpuSlotLease(WarmGpuSlotLease&& other) noexcept :
    owner_(std::exchange(other.owner_, nullptr)), index_(other.index_),
    completed_(other.completed_) {}

WarmGpuSlotLease::~WarmGpuSlotLease() {
    if (owner_ != nullptr) {
        owner_->release_slot(index_, completed_);
    }
}

WarmGpuSlotBuffers WarmGpuSlotLease::buffers() const noexcept {
    return owner_->slot_buffers(index_);
}

std::string WarmGpuSlotLease::ensure_denoise_resources() {
    return owner_->ensure_denoise_resources(index_);
}

std::string WarmGpuSlotLease::ensure_sharpen_resources() {
    return owner_->ensure_sharpen_resources(index_);
}

std::string WarmGpuSlotLease::ensure_clarity_resources() {
    return owner_->ensure_clarity_resources(index_);
}

std::string WarmGpuSlotLease::ensure_texture_clarity_resources() {
    return owner_->ensure_texture_clarity_resources(index_);
}

std::string WarmGpuSlotLease::ensure_local_contrast_resources() {
    return owner_->ensure_local_contrast_resources(index_);
}

std::string WarmGpuSlotLease::ensure_layer_resources() {
    return owner_->ensure_layer_resources(index_);
}

std::string WarmGpuSlotLease::ensure_mask_coverage_resources() {
    return owner_->ensure_mask_coverage_resources(index_);
}

std::string WarmGpuSlotLease::ensure_retouch_resources() {
    if (std::string diagnostic = owner_->ensure_denoise_resources(index_); !diagnostic.empty()) {
        return diagnostic;
    }
    if (std::string diagnostic = owner_->ensure_layer_resources(index_); !diagnostic.empty()) {
        return diagnostic;
    }
    return owner_->ensure_retouch_resources(index_);
}

void WarmGpuSlotLease::mark_completed() noexcept {
    completed_ = true;
}

WarmGpuResidentPreparation prepare_warm_gpu_resident_resources(
    const FloatRgbImage& source,
    id<MTLDevice> device,
    const SensorClippingMask* sensor_clipping_mask,
    const HighlightChromaRiskMap* highlight_chroma_risk_map
) {
    return prepare_warm_gpu_resident_resources(
        source,
        device,
        nil,
        0U,
        static_cast<std::uint64_t>(default_resident_allowance(device)),
        sensor_clipping_mask,
        highlight_chroma_risk_map
    );
}

WarmGpuResidentPreparation prepare_warm_gpu_resident_resources(
    const FloatRgbImage& source,
    id<MTLDevice> device,
    id<MTLBuffer> adopted_source,
    const std::uint64_t external_resident_bytes,
    const std::uint64_t requested_resident_allowance_bytes,
    const SensorClippingMask* sensor_clipping_mask,
    const HighlightChromaRiskMap* highlight_chroma_risk_map
) {
    if (device == nil) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "no Metal device is available",
        };
    }
    const bool adopting = adopted_source != nil;
    if (sensor_clipping_mask != nullptr
        && (!sensor_clipping_mask->valid()
            || sensor_clipping_mask->dimensions != source.dimensions)) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "warm-preview sensor-clipping evidence does not match its source",
        };
    }
    if (highlight_chroma_risk_map != nullptr
        && (!highlight_chroma_risk_map->valid()
            || highlight_chroma_risk_map->dimensions != source.dimensions)) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "warm-preview highlight-chroma evidence does not match its source",
        };
    }
    if (adopting
        && (adopted_source.device == nil
            || static_cast<std::uint64_t>(adopted_source.device.registryID)
                   != static_cast<std::uint64_t>(device.registryID))) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "adopted warm-preview source belongs to a different Metal device",
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
        || source.row_stride_bytes / sizeof(float) > std::numeric_limits<std::uint32_t>::max()
        || source.dimensions.width > std::numeric_limits<std::uint32_t>::max() / 3U
        || !std::isfinite(source.level_zero_to_raster_scale_x)
        || !std::isfinite(source.level_zero_to_raster_scale_y)
        || source.level_zero_to_raster_scale_x <= 0.0
        || source.level_zero_to_raster_scale_y <= 0.0) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
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
        || (!adopting && source.samples.size() != sample_count)
        || (adopting && !source.samples.empty())) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "warm-preview source storage does not match its declared layout",
        };
    }
    if (!adopting) {
        for (const float sample : source.samples) {
            if (!std::isfinite(sample)) {
                return WarmGpuResidentPreparation{
                    .resources = nullptr,
                    .diagnostic = "warm-preview source contains a non-finite sample",
                };
            }
        }
    }

    std::size_t source_bytes = 0U;
    std::size_t clipping_bytes = 0U;
    std::size_t adjusted_sample_count = 0U;
    std::size_t adjusted_bytes = 0U;
    std::size_t rgb8_bytes = 0U;
    if (!checked_multiply(sample_count, sizeof(float), source_bytes)
        || !checked_multiply(
            static_cast<std::size_t>(source.dimensions.pixel_count()),
            sizeof(std::uint8_t),
            clipping_bytes
        )
        || !checked_multiply(
            static_cast<std::size_t>(source.dimensions.pixel_count()),
            3U,
            adjusted_sample_count
        )
        || !checked_multiply(adjusted_sample_count, sizeof(float), adjusted_bytes)
        || !checked_multiply(
            static_cast<std::size_t>(source.dimensions.pixel_count()),
            3U,
            rgb8_bytes
        )
        || source_bytes == 0U || clipping_bytes == 0U || adjusted_bytes == 0U || rgb8_bytes == 0U) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "warm-preview resident Metal buffer size overflowed",
        };
    }
    constexpr std::size_t maximum_shader_sample_index =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (sample_count > maximum_shader_sample_index
        || adjusted_sample_count > maximum_shader_sample_index) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic =
                "warm-preview raster exceeds the Metal kernel's uint32 sample address space",
        };
    }
    constexpr std::size_t operation_buffer_bytes =
        maximum_warm_adjustment_operations * sizeof(MetalAdjustmentOp);
    constexpr std::size_t empty_side_table_bytes = sizeof(MetalCurveSegment);
    const std::size_t maximum_buffer_bytes = static_cast<std::size_t>(device.maxBufferLength);
    if (source_bytes > maximum_buffer_bytes || clipping_bytes > maximum_buffer_bytes
        || adjusted_bytes > maximum_buffer_bytes || rgb8_bytes > maximum_buffer_bytes
        || operation_buffer_bytes > maximum_buffer_bytes
        || empty_side_table_bytes > maximum_buffer_bytes) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "warm-preview resident buffers exceed this Metal device's limit",
        };
    }
    if (adopting && static_cast<std::size_t>(adopted_source.length) < source_bytes) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "adopted warm-preview source buffer is truncated",
        };
    }

    std::size_t per_slot_bytes = 0U;
    std::size_t slots_bytes = 0U;
    std::size_t resident_bytes = 0U;
    if (!checked_add(adjusted_bytes, rgb8_bytes, per_slot_bytes)
        || !checked_add(per_slot_bytes, operation_buffer_bytes, per_slot_bytes)
        || !checked_add(per_slot_bytes, sizeof(WarmStatus), per_slot_bytes)
        || !checked_multiply(per_slot_bytes, warm_slot_count, slots_bytes)
        || !checked_add(source_bytes, clipping_bytes, resident_bytes)
        || !checked_add(resident_bytes, slots_bytes, resident_bytes)
        || !checked_add(resident_bytes, empty_side_table_bytes, resident_bytes)) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "warm-preview resident working-set size overflowed",
        };
    }
    if (external_resident_bytes > std::numeric_limits<std::size_t>::max()
        || requested_resident_allowance_bytes > std::numeric_limits<std::size_t>::max()) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "warm-preview combined Metal budget exceeds the address space",
        };
    }
    const std::size_t resident_allowance_bytes = std::min(
        static_cast<std::size_t>(requested_resident_allowance_bytes),
        default_resident_allowance(device)
    );
    std::size_t combined_resident_bytes = 0U;
    if (resident_allowance_bytes == 0U
        || !checked_add(
            static_cast<std::size_t>(external_resident_bytes),
            resident_bytes,
            combined_resident_bytes
        )
        || combined_resident_bytes > resident_allowance_bytes) {
        return WarmGpuResidentPreparation{
            .resources = nullptr,
            .diagnostic = "warm-preview resident buffers exceed the combined Metal budget",
        };
    }

    auto impl = std::make_unique<WarmGpuResidentResources::Impl>();
    impl->device = device;
    [impl->device retain];
    impl->layout = WarmGpuResidentLayout{
        .dimensions = source.dimensions,
        .source_row_stride_bytes = source.row_stride_bytes,
        .adjusted_row_stride_bytes =
            static_cast<std::size_t>(source.dimensions.width) * 3U * sizeof(float),
        .adjusted_sample_count = adjusted_sample_count,
        .adjusted_bytes = adjusted_bytes,
        .rgb8_bytes = rgb8_bytes,
        .pixel_format = source.pixel_format,
        .transfer_function = source.transfer_function,
        .reference = source.reference,
        .working_space = source.working_space,
        .level_zero_to_raster_scale_x = source.level_zero_to_raster_scale_x,
        .level_zero_to_raster_scale_y = source.level_zero_to_raster_scale_y,
    };
    impl->operation_buffer_bytes = operation_buffer_bytes;
    impl->external_resident_bytes = static_cast<std::size_t>(external_resident_bytes);
    impl->resident_allowance_bytes = resident_allowance_bytes;
    impl->curve_tables.reserve(maximum_resident_curve_tables);
    impl->lut_tables.reserve(maximum_resident_lut_tables);
    impl->perceptual_mixer_tables.reserve(maximum_resident_perceptual_mixer_tables);
    impl->perceptual_range_tables.reserve(maximum_resident_perceptual_range_tables);
    impl->selective_color_tables.reserve(maximum_resident_selective_color_tables);
    impl->brush_index_tables.reserve(maximum_resident_brush_index_tables);
    impl->retouch_geometry_tables.reserve(maximum_resident_retouch_geometry_tables);
    impl->liquify_geometry_tables.reserve(maximum_resident_liquify_geometry_tables);
    impl->stats = WarmEditPreviewGpuStats{
        .resident = true,
        .source_upload_count = adopting ? 0U : 1U,
        .gpu_buffer_allocation_count = (adopting ? 0U : 1U) + 1U + warm_slot_count * 4U,
        .resident_bytes = resident_bytes,
    };

    @autoreleasepool {
        impl->source = adopting ? [adopted_source retain]
                                : [device newBufferWithBytes:source.samples.data()
                                                      length:source_bytes
                                                     options:MTLResourceStorageModeShared];
        if (impl->source == nil) {
            return WarmGpuResidentPreparation{
                .resources = nullptr,
                .diagnostic =
                    adopting ? "Metal could not retain the adopted immutable warm-preview source"
                             : "Metal could not upload the immutable warm-preview source",
            };
        }
        if (sensor_clipping_mask != nullptr || highlight_chroma_risk_map != nullptr) {
            std::vector<std::uint8_t> encoded_evidence(clipping_bytes, 0U);
            for (std::size_t index = 0U; index < encoded_evidence.size(); ++index) {
                const std::uint8_t continuous = highlight_chroma_risk_map != nullptr
                                                    ? highlight_chroma_risk_map->samples[index]
                                                    : 0U;
                const bool physical = sensor_clipping_mask != nullptr
                                      && (sensor_clipping_mask->samples[index]
                                          & sensor_highlight_clipped) != 0U;
                encoded_evidence[index] = physical ? 255U : continuous;
            }
            impl->highlight_clipping = [device newBufferWithBytes:encoded_evidence.data()
                                                            length:clipping_bytes
                                                           options:MTLResourceStorageModeShared];
        } else {
            impl->highlight_clipping = [device newBufferWithLength:clipping_bytes
                                                           options:MTLResourceStorageModeShared];
            if (impl->highlight_clipping != nil) {
                std::memset(impl->highlight_clipping.contents, 0, clipping_bytes);
            }
        }
        if (impl->highlight_clipping == nil) {
            return WarmGpuResidentPreparation{
                .resources = nullptr,
                .diagnostic = "Metal could not retain warm-preview highlight source evidence",
            };
        }
        const MetalCurveSegment empty_side_table{};
        impl->empty_side_table = [device newBufferWithBytes:&empty_side_table
                                                     length:sizeof(empty_side_table)
                                                    options:MTLResourceStorageModeShared];
        if (impl->empty_side_table == nil) {
            return WarmGpuResidentPreparation{
                .resources = nullptr,
                .diagnostic = "Metal could not allocate the empty adjustment side table",
            };
        }
        for (auto& slot : impl->slots) {
            slot.adjusted = [device newBufferWithLength:adjusted_bytes
                                                options:MTLResourceStorageModeShared];
            slot.rgb8 = [device newBufferWithLength:rgb8_bytes
                                            options:MTLResourceStorageModeShared];
            slot.before_operations = [device newBufferWithLength:operation_buffer_bytes
                                                         options:MTLResourceStorageModeShared];
            slot.status = [device newBufferWithLength:sizeof(WarmStatus)
                                              options:MTLResourceStorageModeShared];
            if (slot.adjusted == nil || slot.rgb8 == nil || slot.before_operations == nil
                || slot.status == nil) {
                return WarmGpuResidentPreparation{
                    .resources = nullptr,
                    .diagnostic = "Metal could not allocate both warm-preview execution slots",
                };
            }
        }
    }

    return WarmGpuResidentPreparation{
        .resources = std::unique_ptr<WarmGpuResidentResources>(
            new WarmGpuResidentResources(std::move(impl))
        ),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
