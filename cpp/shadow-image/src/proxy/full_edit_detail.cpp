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
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/working_rgb.hpp>

#include "developed_source_raster.hpp"
#include "full_edit_detail_gpu_cache.hpp"
#include "proxy_render_request_validation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace shadow::image {

namespace {

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

[[nodiscard]] std::uint64_t checked_detail_retained_bytes(const PixelBuffer& source) {
    if (source.samples.capacity()
        > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint16_t)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail retained byte count overflows"
        );
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(source.samples.capacity()) * sizeof(std::uint16_t);
    if (bytes > maximum_full_edit_detail_retained_bytes) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail source exceeds the 512 MiB retained limit"
        );
    }
    return bytes;
}

[[nodiscard]] std::uint64_t checked_detail_retained_bytes(const SceneLinearRgbFrame& source) {
    proxy_detail::validate_developed_source(source);
    const std::uint64_t bytes = static_cast<std::uint64_t>(source.samples.size()) * sizeof(float);
    if (bytes > maximum_full_edit_scene_linear_retained_bytes) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit scene-linear RAW source exceeds the 1 GiB retained-buffer limit"
        );
    }
    return bytes;
}

void preflight_detail_metadata(const AssetMetadata& metadata) {
    const std::uint64_t pixels =
        std::max(metadata.raw_dimensions.pixel_count(), metadata.image_dimensions.pixel_count());
    constexpr std::uint64_t scene_linear_bytes_per_pixel = 3U * sizeof(float);
    if (pixels == 0U
        || pixels > maximum_full_edit_scene_linear_retained_bytes / scene_linear_bytes_per_pixel) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "full edit detail metadata exceeds the 1 GiB worst-case scene-linear RGB limit"
        );
    }
}

struct PreparedReferenceRgb final {
    DevelopedSourcePixels source;
    RawDevelopmentReceipt raw_development_receipt;
    RawPipelineReceipt raw_pipeline_receipt;
    OpticsProfileReceipt optics_receipt;
    SourceRenderingReceipt source_rendering;
};

[[nodiscard]] PreparedReferenceRgb prepare_reference_rgb(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    DevelopedSourceReference developed = develop_source_reference(
        session,
        raw_development_plan,
        std::nullopt,
        raw_pipeline_policy_from_environment()
    );
    DevelopedSourcePixels source = std::move(developed.source);
    // Validate the provider source before source-render normalization inspects its luminance.
    // That preserves the renderer's public typed-error contract for malformed decoded rasters
    // and keeps an invalid transfer/primaries declaration from escaping as std::invalid_argument.
    proxy_detail::validate_developed_source(source);
    // Decoder provenance belongs to the source render, not to a later optical remap. Preserve it
    // independently before passing the buffer to arbitrary provider implementations, which may
    // correctly allocate a new PixelBuffer without knowing Shadow's future sidecar fields.
    RawDevelopmentReceipt raw_development_receipt = std::move(developed.raw_development_receipt);
    // Resolve the source profile from the decoder's standardized raster before handing it to a
    // pluggable optics implementation. Optical adapters are permitted to return an independent
    // pixel allocation; they must not become accidental owners of source-profile provenance.
    const SourceRenderingReceipt source_rendering = std::visit(
        [&](const auto& value) {
            return resolve_source_rendering(value, session.metadata(), developed.pipeline_receipt);
        },
        source
    );
    OpticsProfileReceipt receipt;
    if (optics_provider == nullptr) {
        receipt.status = OpticsProfileStatus::disabled;
        receipt.provider_id = "none";
        receipt.provider_version = "none";
        return {
            .source = std::move(source),
            .raw_development_receipt = std::move(raw_development_receipt),
            .raw_pipeline_receipt = std::move(developed.pipeline_receipt),
            .optics_receipt = std::move(receipt),
            .source_rendering = source_rendering,
        };
    }
    if (std::holds_alternative<PixelBuffer>(source)) {
        auto corrected = optics_provider->correct_reference_rgb(
            std::get<PixelBuffer>(source),
            session.metadata(),
            optics_settings
        );
        receipt = std::move(corrected.receipt);
        if (corrected.corrected_reference_rgb.has_value()) {
            source = std::move(*corrected.corrected_reference_rgb);
        }
    } else {
        auto corrected = optics_provider->correct_scene_linear_reference(
            std::get<SceneLinearRgbFrame>(source),
            session.metadata(),
            optics_settings
        );
        receipt = std::move(corrected.receipt);
        if (corrected.corrected_scene_linear_rgb.has_value()) {
            source = std::move(*corrected.corrected_scene_linear_rgb);
        }
    }
    return {
        .source = std::move(source),
        .raw_development_receipt = std::move(raw_development_receipt),
        .raw_pipeline_receipt = std::move(developed.pipeline_receipt),
        .optics_receipt = std::move(receipt),
        .source_rendering = source_rendering,
    };
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

FullEditDetailSession::FullEditDetailSession(FullEditDetailSession&&) noexcept = default;

FullEditDetailSession& FullEditDetailSession::operator=(FullEditDetailSession&&) noexcept = default;

FullEditDetailSession::~FullEditDetailSession() = default;

Dimensions FullEditDetailSession::dimensions() const noexcept {
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
        if (geometry == PhotoGeometry{}) {
            auto gpu = gpu_cache_->render(
                reference_source_,
                source_rendering_,
                nodes,
                rect,
                working_rect,
                full_dimensions
            );
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
        } else {
            fallback_diagnostic = "photo geometry currently uses the CPU full-detail executor";
        }
        if (requested_backend == AdjustmentBackendMode::metal) {
            throw EditError(EditErrorCode::backend_failure, std::nullopt, fallback_diagnostic);
        }
    }
    FloatRgbImage tile = proxy_detail::crop_developed_source_to_working(
        reference_source_,
        GeometryPixelRect{
            .x = working_rect.x,
            .y = working_rect.y,
            .width = working_rect.width,
            .height = working_rect.height,
        }
    );
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
    if (requested_backend == AdjustmentBackendMode::metal) {
        throw EditError(
            EditErrorCode::backend_failure,
            std::nullopt,
            "local-mask layers currently use the CPU full-detail executor"
        );
    }
    const bool fell_back = requested_backend == AdjustmentBackendMode::automatic;
    FloatRgbImage tile = proxy_detail::crop_developed_source_to_working(
        reference_source_,
        GeometryPixelRect{
            .x = working_rect.x,
            .y = working_rect.y,
            .width = working_rect.width,
            .height = working_rect.height,
        }
    );
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
            fell_back,
            fell_back ? "local-mask layers currently use the CPU full-detail executor" : ""
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
    preflight_detail_metadata(session.metadata());
    if (raw_development_plan.intent != RawDevelopmentIntent::detail
        && raw_development_plan.intent != RawDevelopmentIntent::export_image) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "full-resolution edit source requires detail or export-image intent"
        );
    }
    proxy_detail::validate_raw_development_plan_intent(
        raw_development_plan,
        raw_development_plan.intent,
        raw_development_plan.intent == RawDevelopmentIntent::detail ? "full edit detail"
                                                                    : "full image export"
    );
    auto reference =
        prepare_reference_rgb(session, raw_development_plan, optics_provider, optics_settings);
    const std::uint64_t retained_bytes = std::visit(
        [](const auto& value) { return checked_detail_retained_bytes(value); },
        reference.source
    );
    return FullEditDetailSession(
        std::move(reference.source),
        retained_bytes,
        std::move(reference.raw_development_receipt),
        std::move(reference.raw_pipeline_receipt),
        std::move(reference.optics_receipt),
        std::move(reference.source_rendering)
    );
}

} // namespace shadow::image
