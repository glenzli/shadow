#include "full_edit_detail_gpu_cache.hpp"

#include "developed_source_raster.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace shadow::image::detail {

std::shared_ptr<WarmEditGpuSession> FullEditDetailGpuCache::find_locked(const DetailTileRect rect) {
    const auto match = std::ranges::find(entries_, rect, &Entry::rect);
    if (match == entries_.end()) {
        return nullptr;
    }
    match->last_use = ++use_sequence_;
    return match->session;
}

void FullEditDetailGpuCache::make_room_locked(const std::uint64_t incoming_bytes) {
    if (incoming_bytes == 0U || incoming_bytes > maximum_resident_bytes) {
        return;
    }
    while (!entries_.empty()
           && (entries_.size() >= maximum_entries
               || resident_bytes_ > maximum_resident_bytes - incoming_bytes)) {
        const auto oldest = std::ranges::min_element(entries_, {}, &Entry::last_use);
        if (oldest->resident_bytes > resident_bytes_) {
            entries_.clear();
            resident_bytes_ = 0U;
            return;
        }
        resident_bytes_ -= oldest->resident_bytes;
        entries_.erase(oldest);
    }
}

void FullEditDetailGpuCache::refresh_resident_bytes(
    const DetailTileRect rect,
    const std::shared_ptr<WarmEditGpuSession>& session
) {
    const std::uint64_t current_bytes = session->stats().resident_bytes;
    std::scoped_lock lock(mutex_);
    refresh_resident_bytes_locked(rect, session, current_bytes);
}

void FullEditDetailGpuCache::refresh_resident_bytes_locked(
    const DetailTileRect rect,
    const std::shared_ptr<WarmEditGpuSession>& session,
    const std::uint64_t current_bytes
) {
    const auto match = std::ranges::find(entries_, rect, &Entry::rect);
    if (match == entries_.end() || match->session != session) {
        return;
    }
    if (match->resident_bytes > resident_bytes_) {
        entries_.clear();
        resident_bytes_ = 0U;
        return;
    }
    resident_bytes_ -= match->resident_bytes;
    match->resident_bytes = 0U;
    if (current_bytes == 0U || current_bytes > maximum_resident_bytes) {
        entries_.erase(match);
        return;
    }
    while (resident_bytes_ > maximum_resident_bytes - current_bytes) {
        auto oldest = entries_.end();
        for (auto candidate = entries_.begin(); candidate != entries_.end(); ++candidate) {
            if (candidate->session == session) {
                continue;
            }
            if (oldest == entries_.end() || candidate->last_use < oldest->last_use) {
                oldest = candidate;
            }
        }
        if (oldest == entries_.end()) {
            entries_.clear();
            resident_bytes_ = 0U;
            return;
        }
        if (oldest->resident_bytes > resident_bytes_) {
            entries_.clear();
            resident_bytes_ = 0U;
            return;
        }
        resident_bytes_ -= oldest->resident_bytes;
        entries_.erase(oldest);
    }
    const auto refreshed = std::ranges::find(entries_, rect, &Entry::rect);
    if (refreshed == entries_.end() || refreshed->session != session) {
        resident_bytes_ = 0U;
        entries_.clear();
        return;
    }
    refreshed->resident_bytes = current_bytes;
    resident_bytes_ += current_bytes;
}

FullEditDetailGpuCache::Acquisition FullEditDetailGpuCache::acquire(
    const DevelopedSourcePixels& source,
    const SourceRenderingReceipt& source_rendering,
    const DetailTileRect working_rect
) {
    {
        std::scoped_lock lock(mutex_);
        if (auto session = find_locked(working_rect)) {
            return Acquisition{
                .session = std::move(session),
                .cache_hit = true,
                .diagnostic = {},
            };
        }
    }

    FloatRgbImage working = proxy_detail::crop_developed_source_to_working(
        source,
        GeometryPixelRect{
            .x = working_rect.x,
            .y = working_rect.y,
            .width = working_rect.width,
            .height = working_rect.height,
        }
    );
    apply_source_rendering(working, source_rendering);
    auto preparation = prepare_warm_edit_gpu_session(working);
    if (!preparation.session) {
        return Acquisition{
            .session = nullptr,
            .cache_hit = false,
            .diagnostic = std::move(preparation.diagnostic),
        };
    }

    const std::uint64_t prepared_bytes = preparation.session->stats().resident_bytes;
    std::scoped_lock lock(mutex_);
    if (auto existing = find_locked(working_rect)) {
        return Acquisition{
            .session = std::move(existing),
            .cache_hit = true,
            .diagnostic = {},
        };
    }
    // An unusually expensive working region remains usable for this render but is not retained:
    // one entry must never evict the complete bounded viewport cache and then exceed its budget.
    if (prepared_bytes == 0U || prepared_bytes > maximum_resident_bytes) {
        return Acquisition{
            .session = std::move(preparation.session),
            .cache_hit = false,
            .diagnostic = {},
        };
    }
    make_room_locked(prepared_bytes);
    resident_bytes_ += prepared_bytes;
    entries_.push_back(
        Entry{
            .rect = working_rect,
            .session = preparation.session,
            .resident_bytes = prepared_bytes,
            .last_use = ++use_sequence_,
        }
    );
    return Acquisition{
        .session = std::move(preparation.session),
        .cache_hit = false,
        .diagnostic = {},
    };
}

FullEditDetailGpuCache::RenderAttempt FullEditDetailGpuCache::render(
    const DevelopedSourcePixels& source,
    const SourceRenderingReceipt& source_rendering,
    const std::span<const AdjustmentNode> nodes,
    const DetailTileRect core_rect,
    const DetailTileRect working_rect,
    const Dimensions full_dimensions,
    std::optional<WarmEditGpuGeometryContext> geometry
) {
    const EditExecutionPlan plan = compile_edit_execution_plan(nodes, 1.0, 1.0);
    auto acquisition = acquire(source, source_rendering, working_rect);
    if (!acquisition.session) {
        return RenderAttempt{
            .bytes = std::nullopt,
            .source_cache_hit = false,
            .diagnostic = acquisition.diagnostic.empty()
                              ? "resident Metal full-detail tile is unavailable"
                              : std::move(acquisition.diagnostic),
        };
    }
    const bool geometry_applied = geometry.has_value();
    const std::uint32_t display_origin_x =
        geometry_applied ? geometry->output_rect.x : working_rect.x;
    const std::uint32_t display_origin_y =
        geometry_applied ? geometry->output_rect.y : working_rect.y;
    auto attempt = acquisition.session->render(
        nodes,
        plan,
        false,
        WarmEditGpuRenderContext{
            .adjustment =
                AdjustmentExecutionContext{
                    .origin_x = working_rect.x,
                    .origin_y = working_rect.y,
                    .full_dimensions = full_dimensions,
                },
            .display_origin_x = display_origin_x,
            .display_origin_y = display_origin_y,
            .geometry = std::move(geometry),
        }
    );
    refresh_resident_bytes(working_rect, acquisition.session);
    return finish_render(
        std::move(attempt),
        acquisition.cache_hit,
        core_rect,
        working_rect,
        geometry_applied
    );
}

FullEditDetailGpuCache::RenderAttempt FullEditDetailGpuCache::render_layers(
    const DevelopedSourcePixels& source,
    const SourceRenderingReceipt& source_rendering,
    const std::span<const AdjustmentLayer> layers,
    const DetailTileRect core_rect,
    const DetailTileRect working_rect,
    const Dimensions full_dimensions,
    std::optional<WarmEditGpuGeometryContext> geometry
) {
    auto acquisition = acquire(source, source_rendering, working_rect);
    if (!acquisition.session) {
        return RenderAttempt{
            .bytes = std::nullopt,
            .source_cache_hit = false,
            .diagnostic = acquisition.diagnostic.empty()
                              ? "resident Metal full-detail layer tile is unavailable"
                              : std::move(acquisition.diagnostic),
        };
    }
    const bool geometry_applied = geometry.has_value();
    const std::uint32_t display_origin_x =
        geometry_applied ? geometry->output_rect.x : working_rect.x;
    const std::uint32_t display_origin_y =
        geometry_applied ? geometry->output_rect.y : working_rect.y;
    auto attempt = acquisition.session->render_layers(
        layers,
        false,
        WarmEditGpuRenderContext{
            .adjustment =
                AdjustmentExecutionContext{
                    .origin_x = working_rect.x,
                    .origin_y = working_rect.y,
                    .full_dimensions = full_dimensions,
                },
            .display_origin_x = display_origin_x,
            .display_origin_y = display_origin_y,
            .geometry = std::move(geometry),
        }
    );
    refresh_resident_bytes(working_rect, acquisition.session);
    return finish_render(
        std::move(attempt),
        acquisition.cache_hit,
        core_rect,
        working_rect,
        geometry_applied
    );
}

FullEditDetailGpuCache::RenderAttempt FullEditDetailGpuCache::finish_render(
    WarmEditGpuSession::RenderAttempt attempt,
    const bool source_cache_hit,
    const DetailTileRect core_rect,
    const DetailTileRect working_rect,
    const bool geometry_applied
) {
    if (attempt.status != WarmEditGpuSession::RenderStatus::completed
        || !attempt.output.has_value()) {
        return RenderAttempt{
            .bytes = std::nullopt,
            .source_cache_hit = source_cache_hit,
            .diagnostic = attempt.diagnostic.empty()
                              ? "resident Metal full-detail tile declined the adjustment plan"
                              : std::move(attempt.diagnostic),
        };
    }
    const auto& output = *attempt.output;
    const Dimensions expected_dimensions =
        geometry_applied ? Dimensions{core_rect.width, core_rect.height}
                         : Dimensions{working_rect.width, working_rect.height};
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(expected_dimensions.width) * expected_dimensions.height * 3U;
    if (output.dimensions != expected_dimensions
        || expected_bytes > std::numeric_limits<std::size_t>::max()
        || output.rgb8.size() != static_cast<std::size_t>(expected_bytes)) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "resident Metal full-detail tile returned an invalid RGB8 raster"
        );
    }
    if (geometry_applied) {
        return RenderAttempt{
            .bytes = std::move(attempt.output->rgb8),
            .source_cache_hit = source_cache_hit,
            .diagnostic = {},
        };
    }
    const std::uint32_t core_offset_x = core_rect.x - working_rect.x;
    const std::uint32_t core_offset_y = core_rect.y - working_rect.y;
    const std::size_t output_stride = static_cast<std::size_t>(working_rect.width) * 3U;
    const std::size_t core_stride = static_cast<std::size_t>(core_rect.width) * 3U;
    std::vector<std::uint8_t> core(core_stride * static_cast<std::size_t>(core_rect.height));
    for (std::uint32_t row = 0U; row < core_rect.height; ++row) {
        const std::size_t source_offset =
            (static_cast<std::size_t>(core_offset_y + row) * output_stride)
            + static_cast<std::size_t>(core_offset_x) * 3U;
        const std::size_t target_offset = static_cast<std::size_t>(row) * core_stride;
        std::memcpy(core.data() + target_offset, output.rgb8.data() + source_offset, core_stride);
    }
    return RenderAttempt{
        .bytes = std::move(core),
        .source_cache_hit = source_cache_hit,
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
