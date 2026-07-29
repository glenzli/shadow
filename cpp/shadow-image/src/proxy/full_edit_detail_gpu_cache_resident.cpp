#include "full_edit_detail_gpu_cache.hpp"

#include "full_edit_detail_metal_source.hpp"

#include <shadow/image/edit_execution_plan.hpp>

#include <utility>

namespace shadow::image::detail {

FullEditDetailGpuCache::Acquisition FullEditDetailGpuCache::acquire_resident(
    raw_pipeline_detail::ResidentRawSource& source,
    const SourceRenderingReceipt& source_rendering,
    const DetailTileRect working_rect
) {
    if (!source.device_path_valid()) {
        std::scoped_lock lock(mutex_);
        entries_.clear();
        resident_bytes_ = 0U;
        return Acquisition{
            .session = nullptr,
            .cache_hit = false,
            .diagnostic = "resident Metal RAW source is terminally invalidated",
        };
    }
    {
        std::scoped_lock lock(mutex_);
        if (auto session = find_locked(working_rect)) {
            if (!source.device_path_valid()) {
                entries_.clear();
                resident_bytes_ = 0U;
                return Acquisition{
                    .session = nullptr,
                    .cache_hit = false,
                    .diagnostic = "resident Metal RAW source is terminally invalidated",
                };
            }
            return Acquisition{
                .session = std::move(session),
                .cache_hit = true,
                .diagnostic = {},
            };
        }

        // A resident RAW miss replaces the previous viewport source. Resident renders are
        // serialized by `resident_render_mutex_`, so clearing the entry releases its warm session
        // before the next RAW/optics transaction calculates one combined device allowance.
        entries_.clear();
        resident_bytes_ = 0U;
    }

    auto preparation = prepare_full_edit_detail_metal_source(
        source,
        source_rendering,
        GeometryPixelRect{
            .x = working_rect.x,
            .y = working_rect.y,
            .width = working_rect.width,
            .height = working_rect.height,
        },
        0U
    );
    if (!preparation.published()) {
        return Acquisition{
            .session = nullptr,
            .cache_hit = false,
            .diagnostic = std::move(preparation.diagnostic),
        };
    }

    const std::uint64_t prepared_bytes = preparation.session->stats().resident_bytes;
    std::scoped_lock lock(mutex_);
    if (prepared_bytes != 0U && prepared_bytes <= maximum_resident_bytes) {
        resident_bytes_ = prepared_bytes;
        entries_.push_back(
            Entry{
                .rect = working_rect,
                .session = preparation.session,
                .resident_bytes = prepared_bytes,
                .last_use = ++use_sequence_,
            }
        );
    }
    return Acquisition{
        .session = std::move(preparation.session),
        .cache_hit = false,
        .diagnostic = {},
    };
}

FullEditDetailGpuCache::RenderAttempt FullEditDetailGpuCache::render_resident(
    raw_pipeline_detail::ResidentRawSource& source,
    const SourceRenderingReceipt& source_rendering,
    const std::span<const AdjustmentNode> nodes,
    const DetailTileRect core_rect,
    const DetailTileRect working_rect,
    const Dimensions full_dimensions,
    std::optional<WarmEditGpuGeometryContext> geometry
) {
    std::scoped_lock resident_render_lock(resident_render_mutex_);
    const EditExecutionPlan plan = compile_edit_execution_plan(nodes, 1.0, 1.0);
    auto acquisition = acquire_resident(source, source_rendering, working_rect);
    if (!acquisition.session) {
        return RenderAttempt{
            .bytes = std::nullopt,
            .source_cache_hit = false,
            .diagnostic = acquisition.diagnostic.empty()
                              ? "resident Metal full-detail RAW tile is unavailable"
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

FullEditDetailGpuCache::RenderAttempt FullEditDetailGpuCache::render_resident_layers(
    raw_pipeline_detail::ResidentRawSource& source,
    const SourceRenderingReceipt& source_rendering,
    const std::span<const AdjustmentLayer> layers,
    const DetailTileRect core_rect,
    const DetailTileRect working_rect,
    const Dimensions full_dimensions,
    std::optional<WarmEditGpuGeometryContext> geometry
) {
    std::scoped_lock resident_render_lock(resident_render_mutex_);
    auto acquisition = acquire_resident(source, source_rendering, working_rect);
    if (!acquisition.session) {
        return RenderAttempt{
            .bytes = std::nullopt,
            .source_cache_hit = false,
            .diagnostic = acquisition.diagnostic.empty()
                              ? "resident Metal full-detail RAW layer tile is unavailable"
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

} // namespace shadow::image::detail
