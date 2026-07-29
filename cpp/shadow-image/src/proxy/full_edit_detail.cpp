#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/working_rgb.hpp>

#include "../edit/local_mask_validation.hpp"
#include "../raw/resident_raw_source.hpp"
#include "developed_source_raster.hpp"
#include "full_edit_detail_gpu_cache.hpp"
#include "full_edit_detail_source_preparation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

[[nodiscard]] std::optional<detail::WarmEditGpuGeometryContext> gpu_geometry_context(
    const PhotoGeometry& geometry,
    const PhotoGeometryLayout& layout,
    const DetailTileRect working_rect,
    const GeometryPixelRect output_rect
) {
    if (geometry == PhotoGeometry{}) {
        return std::nullopt;
    }
    return detail::WarmEditGpuGeometryContext{
        .layout = layout,
        .geometry = geometry,
        .source_tile_rect =
            GeometryPixelRect{
                .x = working_rect.x,
                .y = working_rect.y,
                .width = working_rect.width,
                .height = working_rect.height,
            },
        .output_rect = output_rect,
    };
}

void validate_detail_tile_rect(const DetailTileRect rect, const Dimensions full_dimensions) {
    if (rect.width == 0U || rect.height == 0U || rect.width > maximum_edit_detail_tile_side
        || rect.height > maximum_edit_detail_tile_side) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "detail tile width and height must be in 1..=1024"
        );
    }
    if (rect.x >= full_dimensions.width || rect.y >= full_dimensions.height
        || rect.width > full_dimensions.width - rect.x
        || rect.height > full_dimensions.height - rect.y) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "detail tile rectangle must be fully inside the retained image"
        );
    }
}

[[nodiscard]] AdjustmentFootprint
required_detail_apron(const std::span<const AdjustmentNode> nodes) {
    std::uint64_t horizontal = 0U;
    std::uint64_t vertical = 0U;
    // Sequential neighborhood operations propagate boundary dependencies. Summing their
    // supports is the conservative reverse accumulation; taking only the maximum would create
    // seams as soon as two spatial nodes are enabled.
    for (const AdjustmentNode& node : nodes) {
        if (!node.enabled || locality(node.parameters) != AdjustmentLocality::neighborhood) {
            continue;
        }
        const AdjustmentFootprint node_footprint = footprint(node.parameters, 1.0, 1.0);
        horizontal += node_footprint.horizontal_radius;
        vertical += node_footprint.vertical_radius;
        if (horizontal > maximum_edit_detail_total_apron
            || vertical > maximum_edit_detail_total_apron) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "detail adjustment apron exceeds the 512-pixel limit"
            );
        }
    }
    return AdjustmentFootprint{
        .horizontal_radius = static_cast<std::uint32_t>(horizontal),
        .vertical_radius = static_cast<std::uint32_t>(vertical),
    };
}

[[nodiscard]] DetailTileRect expanded_detail_rect(
    const DetailTileRect core,
    const Dimensions full_dimensions,
    const AdjustmentFootprint apron
) {
    const std::uint32_t left = std::min(core.x, apron.horizontal_radius);
    const std::uint32_t top = std::min(core.y, apron.vertical_radius);
    const std::uint32_t available_right = full_dimensions.width - (core.x + core.width);
    const std::uint32_t available_bottom = full_dimensions.height - (core.y + core.height);
    const std::uint32_t right = std::min(available_right, apron.horizontal_radius);
    const std::uint32_t bottom = std::min(available_bottom, apron.vertical_radius);
    const std::uint64_t width = static_cast<std::uint64_t>(core.width) + left + right;
    const std::uint64_t height = static_cast<std::uint64_t>(core.height) + top + bottom;
    if (width > maximum_edit_detail_working_side || height > maximum_edit_detail_working_side
        || width > std::numeric_limits<std::uint32_t>::max()
        || height > std::numeric_limits<std::uint32_t>::max()) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "expanded detail working region exceeds the 2048-pixel side limit"
        );
    }
    const std::uint64_t pixels = width * height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U
        || pixels * 3U > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "expanded detail working allocation exceeds the address space"
        );
    }
    return DetailTileRect{
        .x = core.x - left,
        .y = core.y - top,
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
    };
}

[[nodiscard]] DetailTileExecutionReceipt detail_tile_execution_receipt(
    const DetailTileRenderBackend backend,
    const bool source_cache_hit,
    const bool fell_back,
    std::string diagnostic
) {
    DetailTileExecutionReceipt receipt{
        .backend = backend,
        .backend_version = backend == DetailTileRenderBackend::metal
                               ? detail_tile_metal_backend_version
                               : detail_tile_cpu_backend_version,
        .source_cache_hit = source_cache_hit,
        .fell_back = fell_back,
        .diagnostic = std::move(diagnostic),
    };
    if (!receipt.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "full-detail tile produced an invalid execution receipt"
        );
    }
    return receipt;
}

} // namespace

bool DetailTileExecutionReceipt::valid() const noexcept {
    if (schema_version != detail_tile_execution_receipt_schema_version) {
        return false;
    }
    switch (backend) {
    case DetailTileRenderBackend::cpu:
        if (backend_version != detail_tile_cpu_backend_version || source_cache_hit) {
            return false;
        }
        break;
    case DetailTileRenderBackend::metal:
        if (backend_version != detail_tile_metal_backend_version || fell_back) {
            return false;
        }
        break;
    }
    return fell_back ? !diagnostic.empty() : diagnostic.empty();
}

FullEditDetailSession::FullEditDetailSession(
    DevelopedSourcePixels reference_source,
    const std::uint64_t retained_bytes,
    RawDevelopmentReceipt raw_development_receipt,
    RawPipelineReceipt raw_pipeline_receipt,
    OpticsProfileReceipt optics_receipt,
    SourceRenderingReceipt source_rendering
) :
    reference_source_(std::move(reference_source)), retained_bytes_(retained_bytes),
    raw_development_receipt_(std::move(raw_development_receipt)),
    raw_pipeline_receipt_(std::move(raw_pipeline_receipt)),
    optics_receipt_(std::move(optics_receipt)), source_rendering_(std::move(source_rendering)),
    gpu_cache_(std::make_unique<detail::FullEditDetailGpuCache>()) {}

FullEditDetailSession::FullEditDetailSession(
    std::unique_ptr<raw_pipeline_detail::ResidentRawSource> resident_raw_source,
    const std::uint64_t retained_bytes,
    RawDevelopmentReceipt raw_development_receipt,
    RawPipelineReceipt raw_pipeline_receipt,
    OpticsProfileReceipt optics_receipt,
    SourceRenderingReceipt source_rendering
) :
    resident_raw_source_(std::move(resident_raw_source)), retained_bytes_(retained_bytes),
    raw_development_receipt_(std::move(raw_development_receipt)),
    raw_pipeline_receipt_(std::move(raw_pipeline_receipt)),
    optics_receipt_(std::move(optics_receipt)), source_rendering_(std::move(source_rendering)),
    gpu_cache_(std::make_unique<detail::FullEditDetailGpuCache>()) {
    if (resident_raw_source_ == nullptr) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "full edit detail received an empty resident RAW source"
        );
    }
}

FullEditDetailSession::FullEditDetailSession(FullEditDetailSession&&) noexcept = default;

FullEditDetailSession& FullEditDetailSession::operator=(FullEditDetailSession&&) noexcept = default;

FullEditDetailSession::~FullEditDetailSession() = default;

Dimensions FullEditDetailSession::dimensions() const noexcept {
    if (resident_raw_source_ != nullptr) {
        return resident_raw_source_->dimensions();
    }
    return proxy_detail::developed_source_dimensions(reference_source_);
}

std::uint64_t FullEditDetailSession::retained_bytes() const noexcept {
    return retained_bytes_;
}

const RawDevelopmentReceipt& FullEditDetailSession::raw_development_receipt() const noexcept {
    return raw_development_receipt_;
}

const RawPipelineReceipt& FullEditDetailSession::raw_pipeline_receipt() const noexcept {
    return raw_pipeline_receipt_;
}

const OpticsProfileReceipt& FullEditDetailSession::optics_receipt() const noexcept {
    return optics_receipt_;
}

RenderedDetailTile FullEditDetailSession::render_rgb8(
    const std::span<const AdjustmentNode> nodes,
    const DetailTileRect rect,
    const PhotoGeometry& geometry
) const {
    validate_adjustment_nodes(nodes);
    const Dimensions full_dimensions = dimensions();
    const PhotoGeometryLayout geometry_layout = photo_geometry_layout(full_dimensions, geometry);
    validate_detail_tile_rect(rect, geometry_layout.output_dimensions);
    const GeometryPixelRect output_rect{rect.x, rect.y, rect.width, rect.height};
    const GeometryPixelRect source_core =
        photo_geometry_source_rect_for_output(geometry_layout, geometry, output_rect);
    const AdjustmentFootprint apron = required_detail_apron(nodes);
    const DetailTileRect working_rect = expanded_detail_rect(
        DetailTileRect{
            .x = source_core.x,
            .y = source_core.y,
            .width = source_core.width,
            .height = source_core.height,
        },
        full_dimensions,
        apron
    );
    const AdjustmentBackendMode requested_backend = adjustment_backend_mode_from_environment();
    std::string fallback_diagnostic;
    if (requested_backend != AdjustmentBackendMode::cpu) {
        auto gpu =
            resident_raw_source_ != nullptr && resident_raw_source_->metal_resident()
                ? gpu_cache_->render_resident(
                      *resident_raw_source_,
                      source_rendering_,
                      nodes,
                      rect,
                      working_rect,
                      full_dimensions,
                      gpu_geometry_context(geometry, geometry_layout, working_rect, output_rect)
                  )
            : resident_raw_source_ == nullptr
                ? gpu_cache_->render(
                      reference_source_,
                      source_rendering_,
                      nodes,
                      rect,
                      working_rect,
                      full_dimensions,
                      gpu_geometry_context(geometry, geometry_layout, working_rect, output_rect)
                  )
                : detail::FullEditDetailGpuCache::RenderAttempt{
                      .bytes = std::nullopt,
                      .source_cache_hit = false,
                      .diagnostic =
                          "resident RAW detail was prepared for the forced CPU development path",
                  };
        if (gpu.bytes.has_value()) {
            return RenderedDetailTile{
                .rect = rect,
                .full_dimensions = geometry_layout.output_dimensions,
                .row_stride_bytes = rect.width * 3U,
                .bytes = std::move(*gpu.bytes),
                .execution = detail_tile_execution_receipt(
                    DetailTileRenderBackend::metal,
                    gpu.source_cache_hit,
                    false,
                    {}
                ),
            };
        }
        fallback_diagnostic = std::move(gpu.diagnostic);
        if (resident_raw_source_ != nullptr && resident_raw_source_->metal_resident()) {
            throw EditError(EditErrorCode::backend_failure, std::nullopt, fallback_diagnostic);
        }
        if (requested_backend == AdjustmentBackendMode::metal) {
            throw EditError(EditErrorCode::backend_failure, std::nullopt, fallback_diagnostic);
        }
    } else if (resident_raw_source_ != nullptr && resident_raw_source_->metal_resident()) {
        throw EditError(
            EditErrorCode::backend_failure,
            std::nullopt,
            "a published Metal-resident RAW source cannot be replayed through the CPU tile path"
        );
    }
    const GeometryPixelRect working_geometry{
        .x = working_rect.x,
        .y = working_rect.y,
        .width = working_rect.width,
        .height = working_rect.height,
    };
    FloatRgbImage tile;
    if (resident_raw_source_ != nullptr) {
        auto developed = resident_raw_source_->develop_region(working_geometry);
        tile = proxy_detail::take_scene_linear_region_to_working(
            std::move(developed.scene_linear),
            full_dimensions
        );
    } else {
        tile = proxy_detail::crop_developed_source_to_working(reference_source_, working_geometry);
    }
    apply_source_rendering(tile, source_rendering_);
    const FloatRgbImage edited_working = execute_adjustment_nodes(
        tile,
        nodes,
        AdjustmentExecutionContext{
            .origin_x = working_rect.x,
            .origin_y = working_rect.y,
            .full_dimensions = full_dimensions,
        }
    );
    const FloatRgbImage edited = apply_photo_geometry_tile(
        edited_working,
        GeometryPixelRect{
            .x = working_rect.x,
            .y = working_rect.y,
            .width = working_rect.width,
            .height = working_rect.height,
        },
        geometry_layout,
        geometry,
        output_rect
    );
    auto rendered = render_linear_srgb_to_display_srgb8_with_backend(
        edited,
        DisplayOutputRequest{
            .target_dimensions = edited.dimensions,
            .output_origin_x = rect.x,
            .output_origin_y = rect.y,
        },
        DisplayOutputBackendMode::cpu
    );
    return RenderedDetailTile{
        .rect = rect,
        .full_dimensions = geometry_layout.output_dimensions,
        .row_stride_bytes = rect.width * 3U,
        .bytes = std::move(rendered.bytes),
        .execution = detail_tile_execution_receipt(
            DetailTileRenderBackend::cpu,
            false,
            !fallback_diagnostic.empty(),
            std::move(fallback_diagnostic)
        ),
    };
}

RenderedDetailTile FullEditDetailSession::render_rgb8_layers(
    const std::span<const AdjustmentLayer> layers,
    const DetailTileRect rect,
    const PhotoGeometry& geometry
) const {
    const Dimensions full_dimensions = dimensions();
    static_cast<void>(detail::validate_adjustment_layer_plan(
        full_dimensions,
        layers,
        AdjustmentExecutionContext{.full_dimensions = full_dimensions}
    ));
    const PhotoGeometryLayout geometry_layout = photo_geometry_layout(full_dimensions, geometry);
    validate_detail_tile_rect(rect, geometry_layout.output_dimensions);
    const GeometryPixelRect output_rect{rect.x, rect.y, rect.width, rect.height};
    const GeometryPixelRect source_core =
        photo_geometry_source_rect_for_output(geometry_layout, geometry, output_rect);
    std::vector<AdjustmentNode> flattened_nodes;
    for (const auto& layer : layers) {
        flattened_nodes.insert(flattened_nodes.end(), layer.nodes.begin(), layer.nodes.end());
    }
    const AdjustmentFootprint apron = required_detail_apron(flattened_nodes);
    const DetailTileRect working_rect = expanded_detail_rect(
        DetailTileRect{
            .x = source_core.x,
            .y = source_core.y,
            .width = source_core.width,
            .height = source_core.height,
        },
        full_dimensions,
        apron
    );
    const AdjustmentBackendMode requested_backend = adjustment_backend_mode_from_environment();
    std::string fallback_diagnostic;
    if (requested_backend != AdjustmentBackendMode::cpu) {
        auto gpu =
            resident_raw_source_ != nullptr && resident_raw_source_->metal_resident()
                ? gpu_cache_->render_resident_layers(
                      *resident_raw_source_,
                      source_rendering_,
                      layers,
                      rect,
                      working_rect,
                      full_dimensions,
                      gpu_geometry_context(geometry, geometry_layout, working_rect, output_rect)
                  )
            : resident_raw_source_ == nullptr
                ? gpu_cache_->render_layers(
                      reference_source_,
                      source_rendering_,
                      layers,
                      rect,
                      working_rect,
                      full_dimensions,
                      gpu_geometry_context(geometry, geometry_layout, working_rect, output_rect)
                  )
                : detail::FullEditDetailGpuCache::RenderAttempt{
                      .bytes = std::nullopt,
                      .source_cache_hit = false,
                      .diagnostic =
                          "resident RAW detail was prepared for the forced CPU development path",
                  };
        if (gpu.bytes.has_value()) {
            return RenderedDetailTile{
                .rect = rect,
                .full_dimensions = geometry_layout.output_dimensions,
                .row_stride_bytes = rect.width * 3U,
                .bytes = std::move(*gpu.bytes),
                .execution = detail_tile_execution_receipt(
                    DetailTileRenderBackend::metal,
                    gpu.source_cache_hit,
                    false,
                    {}
                ),
            };
        }
        fallback_diagnostic = std::move(gpu.diagnostic);
        if (resident_raw_source_ != nullptr && resident_raw_source_->metal_resident()) {
            throw EditError(EditErrorCode::backend_failure, std::nullopt, fallback_diagnostic);
        }
        if (requested_backend == AdjustmentBackendMode::metal) {
            throw EditError(EditErrorCode::backend_failure, std::nullopt, fallback_diagnostic);
        }
    } else if (resident_raw_source_ != nullptr && resident_raw_source_->metal_resident()) {
        throw EditError(
            EditErrorCode::backend_failure,
            std::nullopt,
            "a published Metal-resident RAW source cannot be replayed through the CPU layer path"
        );
    }
    const GeometryPixelRect working_geometry{
        .x = working_rect.x,
        .y = working_rect.y,
        .width = working_rect.width,
        .height = working_rect.height,
    };
    FloatRgbImage tile;
    if (resident_raw_source_ != nullptr) {
        auto developed = resident_raw_source_->develop_region(working_geometry);
        tile = proxy_detail::take_scene_linear_region_to_working(
            std::move(developed.scene_linear),
            full_dimensions
        );
    } else {
        tile = proxy_detail::crop_developed_source_to_working(reference_source_, working_geometry);
    }
    apply_source_rendering(tile, source_rendering_);
    const FloatRgbImage edited_working = execute_adjustment_layers(
        tile,
        layers,
        AdjustmentExecutionContext{
            .origin_x = working_rect.x,
            .origin_y = working_rect.y,
            .full_dimensions = full_dimensions,
        }
    );
    const FloatRgbImage edited = apply_photo_geometry_tile(
        edited_working,
        GeometryPixelRect{
            .x = working_rect.x,
            .y = working_rect.y,
            .width = working_rect.width,
            .height = working_rect.height,
        },
        geometry_layout,
        geometry,
        output_rect
    );
    auto rendered = render_linear_srgb_to_display_srgb8_with_backend(
        edited,
        DisplayOutputRequest{
            .target_dimensions = edited.dimensions,
            .output_origin_x = rect.x,
            .output_origin_y = rect.y,
        },
        DisplayOutputBackendMode::cpu
    );
    return RenderedDetailTile{
        .rect = rect,
        .full_dimensions = geometry_layout.output_dimensions,
        .row_stride_bytes = rect.width * 3U,
        .bytes = std::move(rendered.bytes),
        .execution = detail_tile_execution_receipt(
            DetailTileRenderBackend::cpu,
            false,
            !fallback_diagnostic.empty(),
            std::move(fallback_diagnostic)
        ),
    };
}

FullEditDetailSession prepare_full_edit_detail(
    const DecodeSession& session,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    return prepare_full_edit_detail(
        session,
        default_raw_development_plan(),
        optics_provider,
        optics_settings
    );
}

FullEditDetailSession prepare_full_edit_detail(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    auto prepared = proxy_detail::prepare_full_edit_detail_source(
        session,
        raw_development_plan,
        optics_provider,
        optics_settings
    );
    if (prepared.resident()) {
        return FullEditDetailSession(
            std::make_unique<raw_pipeline_detail::ResidentRawSource>(
                std::move(std::get<raw_pipeline_detail::ResidentRawSource>(prepared.source))
            ),
            prepared.retained_bytes,
            std::move(prepared.raw_development_receipt),
            std::move(prepared.raw_pipeline_receipt),
            std::move(prepared.optics_receipt),
            std::move(prepared.source_rendering)
        );
    }
    return FullEditDetailSession(
        std::move(std::get<DevelopedSourcePixels>(prepared.source)),
        prepared.retained_bytes,
        std::move(prepared.raw_development_receipt),
        std::move(prepared.raw_pipeline_receipt),
        std::move(prepared.optics_receipt),
        std::move(prepared.source_rendering)
    );
}

} // namespace shadow::image
