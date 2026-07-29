#pragma once

#include <shadow/image/full_edit_detail.hpp>

#include "warm_edit_gpu.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shadow::image::detail {

// Bounded LRU of expanded full-detail working regions. Each entry retains one immutable Metal
// source upload so repeated recipe changes over the same viewport do not re-upload float RGB.
// The full-detail session owns source semantics; this class owns only transient acceleration.
class FullEditDetailGpuCache final {
  public:
    struct RenderAttempt final {
        std::optional<std::vector<std::uint8_t>> bytes;
        bool source_cache_hit = false;
        std::string diagnostic;
    };

    [[nodiscard]] RenderAttempt render(
        const DevelopedSourcePixels& source,
        const SourceRenderingReceipt& source_rendering,
        std::span<const AdjustmentNode> nodes,
        DetailTileRect core_rect,
        DetailTileRect working_rect,
        Dimensions full_dimensions
    );

  private:
    struct Acquisition final {
        std::shared_ptr<WarmEditGpuSession> session;
        bool cache_hit = false;
        std::string diagnostic;
    };

    [[nodiscard]] Acquisition acquire(
        const DevelopedSourcePixels& source,
        const SourceRenderingReceipt& source_rendering,
        DetailTileRect working_rect
    );

    struct Entry final {
        DetailTileRect rect;
        std::shared_ptr<WarmEditGpuSession> session;
        std::uint64_t resident_bytes = 0U;
        std::uint64_t last_use = 0U;
    };

    [[nodiscard]] std::shared_ptr<WarmEditGpuSession> find_locked(DetailTileRect rect);
    void make_room_locked(std::uint64_t incoming_bytes);
    void
    refresh_resident_bytes(DetailTileRect rect, const std::shared_ptr<WarmEditGpuSession>& session);

    static constexpr std::uint64_t maximum_resident_bytes = 256ULL * 1'024ULL * 1'024ULL;
    static constexpr std::size_t maximum_entries = 4U;

    std::mutex mutex_;
    std::vector<Entry> entries_;
    std::uint64_t resident_bytes_ = 0U;
    std::uint64_t use_sequence_ = 0U;
};

} // namespace shadow::image::detail
