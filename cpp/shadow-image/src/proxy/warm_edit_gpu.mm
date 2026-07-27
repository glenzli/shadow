// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_kernel_contract.hpp"
#include "warm_edit_gpu_msl.hpp"
#include "../edit/adjustment_execution_internal.hpp"
#include "../edit/metal_adjustment_msl.hpp"

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

struct WarmTechnicalDetailStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::optional<WarmDenoiseParameters> denoise;
    std::optional<WarmSharpenParameters> sharpen;
};

// Texture is a color-grading detail component, but the following color wheels in that same node
// are pixel-local. Keep a copy with Texture zeroed for the post stage so the GPU preserves the
// CPU order: Oklab-L texture first, color wheels second, then later nodes.
struct WarmTextureStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmTextureParameters parameters;
};

struct WarmClarityStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmGaussianParameters small_gaussian;
    WarmGaussianParameters large_gaussian;
    WarmClarityParameters parameters;
};

struct WarmDehazeDefringeStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmDehazeDefringeParameters parameters;
};

struct WarmTextureClarityStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmGaussianParameters texture_gaussian;
    WarmGaussianParameters clarity_small_gaussian;
    WarmGaussianParameters clarity_large_gaussian;
    WarmTextureClarityParameters parameters;
};

struct WarmLocalContrastStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmBoxParameters small_box;
    WarmBoxParameters large_box;
    WarmGuidedCoefficientsParameters small_coefficients;
    WarmGuidedCoefficientsParameters large_coefficients;
    WarmLocalContrastParameters parameters;
};

[[nodiscard]] bool is_gpu_warm_technical_detail_supported(
    const SharpenAdjustment& parameters
) noexcept {
    return parameters.execution_pass == DetailEffectsExecutionPass::technical_detail
        && (parameters.denoise_luminance > 0.0 || parameters.denoise_color > 0.0
            || parameters.amount > 0.0)
        && parameters.dehaze == 0.0
        && parameters.defringe_purple_amount == 0.0
        && parameters.defringe_green_amount == 0.0;
}

[[nodiscard]] std::optional<WarmTechnicalDetailStage> prepare_warm_technical_detail_stage(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const Dimensions dimensions,
    const WorkingRgbSpace& working_space,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y
) {
    std::optional<std::size_t> neighbourhood_segment;
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (plan.segments[index].locality == AdjustmentLocality::neighborhood) {
            if (neighbourhood_segment.has_value()) {
                return std::nullopt;
            }
            neighbourhood_segment = index;
        }
    }
    if (!neighbourhood_segment.has_value()) {
        return std::nullopt;
    }
    const EditExecutionSegment& segment = plan.segments[*neighbourhood_segment];
    if (segment.steps.size() != 1U) {
        return std::nullopt;
    }
    const EditExecutionStep& step = segment.steps.front();
    if (step.operation != AdjustmentOperation::sharpen || step.node_index >= nodes.size()) {
        return std::nullopt;
    }
    const auto* detail = std::get_if<SharpenAdjustment>(&nodes[step.node_index].parameters);
    if (detail == nullptr || !is_gpu_warm_technical_detail_supported(*detail)
        || working_space.luminance_coefficients[1] <= 0.0) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (index == *neighbourhood_segment) {
            continue;
        }
        if (plan.segments[index].locality != AdjustmentLocality::pixel_local) {
            return std::nullopt;
        }
    }

    const double strength = std::max(
        detail->denoise_luminance,
        detail->denoise_color
    );
    const double authority = std::clamp(
        strength * strength * (1.0 - 0.60 * detail->denoise_detail),
        0.0,
        1.0
    );
    WarmTechnicalDetailStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
    };
    if (detail->denoise_luminance > 0.0 || detail->denoise_color > 0.0) {
        result.denoise = WarmDenoiseParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .radius = static_cast<std::uint32_t>(std::clamp(
                std::ceil(1.0 + 3.0 * authority),
                1.0,
                4.0
            )),
            // A single edge-aware pass gives the normal interactive controls a light hand. At
            // the top end, run the exact same bilateral stage once more while both rasters are
            // already resident. This keeps a 100% request visibly decisive without widening a
            // single support enough to bleed across the mast, horizon, or specular highlights.
            .passes = authority >= 0.70 ? 2U : 1U,
            .luminance_strength = static_cast<float>(detail->denoise_luminance),
            .color_strength = static_cast<float>(detail->denoise_color),
            .spatial_sigma = static_cast<float>(0.90 + 0.52 * authority),
            // A high ISO warm proxy needs enough range tolerance to identify independent
            // pixel noise as a flat region, while the luma edge term still protects real
            // subject boundaries. This mirrors the CPU path's epsilon authority mapping.
            .edge_sigma = static_cast<float>(0.010 + 0.085 * authority),
            .red_luminance = static_cast<float>(working_space.luminance_coefficients[0]),
            .green_luminance = static_cast<float>(working_space.luminance_coefficients[1]),
            .blue_luminance = static_cast<float>(working_space.luminance_coefficients[2]),
        };
    }
    if (detail->amount > 0.0) {
        const double radius_x = std::ceil(
            3.0 * detail->radius * level_zero_to_raster_scale_x
        );
        const double radius_y = std::ceil(
            3.0 * detail->radius * level_zero_to_raster_scale_y
        );
        // The shader intentionally has a fixed loop bound, keeping its resource and execution
        // footprint predictable. A non-preview-scale source that needs wider support simply
        // declines and replays through the complete CPU oracle.
        if (radius_x > 15.0 || radius_y > 15.0) {
            return std::nullopt;
        }
        result.sharpen = WarmSharpenParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .horizontal_radius = static_cast<std::uint32_t>(radius_x),
            .vertical_radius = static_cast<std::uint32_t>(radius_y),
            .sigma_x = static_cast<float>(
                detail->radius * level_zero_to_raster_scale_x
            ),
            .sigma_y = static_cast<float>(
                detail->radius * level_zero_to_raster_scale_y
            ),
            .amount = static_cast<float>(detail->amount),
            .threshold_ev = static_cast<float>(detail->threshold * 0.25),
            .masking = static_cast<float>(detail->masking),
            .red_luminance = static_cast<float>(working_space.luminance_coefficients[0]),
            .green_luminance = static_cast<float>(working_space.luminance_coefficients[1]),
            .blue_luminance = static_cast<float>(working_space.luminance_coefficients[2]),
        };
    }
    result.before.segments.insert(
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    result.after.segments.insert(
        result.after.segments.end(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment + 1U),
        plan.segments.end()
    );
    return result;
}

[[nodiscard]] std::optional<WarmTextureStage> prepare_warm_texture_stage(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const Dimensions dimensions,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y
) {
    std::optional<std::size_t> neighbourhood_segment;
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (plan.segments[index].locality == AdjustmentLocality::neighborhood) {
            if (neighbourhood_segment.has_value()) {
                return std::nullopt;
            }
            neighbourhood_segment = index;
        }
    }
    if (!neighbourhood_segment.has_value()) {
        return std::nullopt;
    }
    const EditExecutionSegment& segment = plan.segments[*neighbourhood_segment];
    if (segment.steps.size() != 1U) {
        return std::nullopt;
    }
    const EditExecutionStep& step = segment.steps.front();
    if (step.operation != AdjustmentOperation::sharpen || step.node_index >= nodes.size()) {
        return std::nullopt;
    }
    const auto* detail = std::get_if<SharpenAdjustment>(&nodes[step.node_index].parameters);
    if (detail == nullptr
        || detail->execution_pass != DetailEffectsExecutionPass::color_grading
        || detail->texture == 0.0 || detail->clarity != 0.0
        || detail->local_contrast != 0.0) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (index != *neighbourhood_segment
            && plan.segments[index].locality != AdjustmentLocality::pixel_local) {
            return std::nullopt;
        }
    }
    constexpr double texture_sigma_level_zero = 1.4;
    const double sigma_x = texture_sigma_level_zero * level_zero_to_raster_scale_x;
    const double sigma_y = texture_sigma_level_zero * level_zero_to_raster_scale_y;
    const double radius_x = std::ceil(3.0 * sigma_x);
    const double radius_y = std::ceil(3.0 * sigma_y);
    if (radius_x > 15.0 || radius_y > 15.0) {
        return std::nullopt;
    }

    WarmTextureStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .post_nodes = std::vector<AdjustmentNode>(nodes.begin(), nodes.end()),
        .parameters = WarmTextureParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .horizontal_radius = static_cast<std::uint32_t>(radius_x),
            .vertical_radius = static_cast<std::uint32_t>(radius_y),
            .sigma_x = static_cast<float>(sigma_x),
            .sigma_y = static_cast<float>(sigma_y),
            .amount = static_cast<float>(detail->texture),
        },
    };
    auto& post_detail = std::get<SharpenAdjustment>(
        result.post_nodes[step.node_index].parameters
    );
    post_detail.texture = 0.0;

    result.before.segments.insert(
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    // Recompile only the post portion. Keep one explicit replacement step even when all wheels
    // are neutral: it obtains the validated working-space transform for the Texture kernels and
    // is an exact no-op in the final pixel-local interpreter.
    result.after.segments.push_back(EditExecutionSegment{
        .locality = AdjustmentLocality::pixel_local,
        .first_node_index = step.node_index,
        .past_last_node_index = step.node_index + 1U,
        .steps = {EditExecutionStep{
            .node_index = step.node_index,
            .operation = AdjustmentOperation::sharpen,
        }},
    });
    const EditExecutionPlan post_plan = compile_edit_execution_plan(
        result.post_nodes,
        level_zero_to_raster_scale_x,
        level_zero_to_raster_scale_y
    );
    for (const auto& post_segment : post_plan.segments) {
        EditExecutionSegment retained{
            .locality = post_segment.locality,
            .first_node_index = post_segment.first_node_index,
            .past_last_node_index = post_segment.past_last_node_index,
        };
        for (const auto& post_step : post_segment.steps) {
            if (post_step.node_index > step.node_index) {
                retained.steps.push_back(post_step);
            }
        }
        if (!retained.steps.empty()) {
            retained.first_node_index = retained.steps.front().node_index;
            retained.past_last_node_index = retained.steps.back().node_index + 1U;
            result.after.segments.push_back(std::move(retained));
        }
    }
    return result;
}

[[nodiscard]] std::optional<WarmClarityStage> prepare_warm_clarity_stage(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const Dimensions dimensions,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y
) {
    std::optional<std::size_t> neighbourhood_segment;
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (plan.segments[index].locality == AdjustmentLocality::neighborhood) {
            if (neighbourhood_segment.has_value()) {
                return std::nullopt;
            }
            neighbourhood_segment = index;
        }
    }
    if (!neighbourhood_segment.has_value()) {
        return std::nullopt;
    }
    const EditExecutionSegment& segment = plan.segments[*neighbourhood_segment];
    if (segment.steps.size() != 1U) {
        return std::nullopt;
    }
    const EditExecutionStep& step = segment.steps.front();
    if (step.operation != AdjustmentOperation::sharpen || step.node_index >= nodes.size()) {
        return std::nullopt;
    }
    const auto* detail = std::get_if<SharpenAdjustment>(&nodes[step.node_index].parameters);
    if (detail == nullptr
        || detail->execution_pass != DetailEffectsExecutionPass::color_grading
        || detail->clarity == 0.0 || detail->texture != 0.0
        || detail->local_contrast != 0.0) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (index != *neighbourhood_segment
            && plan.segments[index].locality != AdjustmentLocality::pixel_local) {
            return std::nullopt;
        }
    }

    constexpr double small_sigma_level_zero = 2.4;
    constexpr double large_sigma_level_zero = 12.0;
    const double small_sigma_x = small_sigma_level_zero * level_zero_to_raster_scale_x;
    const double small_sigma_y = small_sigma_level_zero * level_zero_to_raster_scale_y;
    const double large_sigma_x = large_sigma_level_zero * level_zero_to_raster_scale_x;
    const double large_sigma_y = large_sigma_level_zero * level_zero_to_raster_scale_y;
    const double small_radius_x = std::ceil(3.0 * small_sigma_x);
    const double small_radius_y = std::ceil(3.0 * small_sigma_y);
    const double large_radius_x = std::ceil(3.0 * large_sigma_x);
    const double large_radius_y = std::ceil(3.0 * large_sigma_y);
    // The resident shader deliberately has a fixed bound. The stage is intended for preview
    // rasters; full-size output and an unusually large proxy simply use the exact CPU oracle.
    if (small_radius_x > 15.0 || small_radius_y > 15.0
        || large_radius_x > 15.0 || large_radius_y > 15.0) {
        return std::nullopt;
    }

    WarmClarityStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .post_nodes = std::vector<AdjustmentNode>(nodes.begin(), nodes.end()),
        .small_gaussian = WarmGaussianParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .horizontal_radius = static_cast<std::uint32_t>(small_radius_x),
            .vertical_radius = static_cast<std::uint32_t>(small_radius_y),
            .sigma_x = static_cast<float>(small_sigma_x),
            .sigma_y = static_cast<float>(small_sigma_y),
        },
        .large_gaussian = WarmGaussianParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .horizontal_radius = static_cast<std::uint32_t>(large_radius_x),
            .vertical_radius = static_cast<std::uint32_t>(large_radius_y),
            .sigma_x = static_cast<float>(large_sigma_x),
            .sigma_y = static_cast<float>(large_sigma_y),
        },
        .parameters = WarmClarityParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .vertical_radius = static_cast<std::uint32_t>(large_radius_y),
            .sigma_y = static_cast<float>(large_sigma_y),
            .amount = static_cast<float>(detail->clarity),
        },
    };
    auto& post_detail = std::get<SharpenAdjustment>(
        result.post_nodes[step.node_index].parameters
    );
    post_detail.clarity = 0.0;

    result.before.segments.insert(
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    // The zeroed replacement preserves the CPU ordering: Clarity first, color wheels in this
    // node second, then all subsequent pixel-local nodes.
    result.after.segments.push_back(EditExecutionSegment{
        .locality = AdjustmentLocality::pixel_local,
        .first_node_index = step.node_index,
        .past_last_node_index = step.node_index + 1U,
        .steps = {EditExecutionStep{
            .node_index = step.node_index,
            .operation = AdjustmentOperation::sharpen,
        }},
    });
    const EditExecutionPlan post_plan = compile_edit_execution_plan(
        result.post_nodes,
        level_zero_to_raster_scale_x,
        level_zero_to_raster_scale_y
    );
    for (const auto& post_segment : post_plan.segments) {
        EditExecutionSegment retained{
            .locality = post_segment.locality,
            .first_node_index = post_segment.first_node_index,
            .past_last_node_index = post_segment.past_last_node_index,
        };
        for (const auto& post_step : post_segment.steps) {
            if (post_step.node_index > step.node_index) {
                retained.steps.push_back(post_step);
            }
        }
        if (!retained.steps.empty()) {
            retained.first_node_index = retained.steps.front().node_index;
            retained.past_last_node_index = retained.steps.back().node_index + 1U;
            result.after.segments.push_back(std::move(retained));
        }
    }
    return result;
}

[[nodiscard]] std::optional<WarmTextureClarityStage> prepare_warm_texture_clarity_stage(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const Dimensions dimensions,
    const double scale_x,
    const double scale_y
) {
    std::optional<std::size_t> neighbourhood_segment;
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (plan.segments[index].locality == AdjustmentLocality::neighborhood) {
            if (neighbourhood_segment.has_value()) {
                return std::nullopt;
            }
            neighbourhood_segment = index;
        }
    }
    if (!neighbourhood_segment.has_value()) {
        return std::nullopt;
    }
    const auto& segment = plan.segments[*neighbourhood_segment];
    if (segment.steps.size() != 1U) {
        return std::nullopt;
    }
    const auto& step = segment.steps.front();
    if (step.operation != AdjustmentOperation::sharpen || step.node_index >= nodes.size()) {
        return std::nullopt;
    }
    const auto* detail = std::get_if<SharpenAdjustment>(&nodes[step.node_index].parameters);
    if (detail == nullptr || detail->execution_pass != DetailEffectsExecutionPass::color_grading
        || detail->texture == 0.0 || detail->clarity == 0.0
        || detail->local_contrast != 0.0) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (index != *neighbourhood_segment
            && plan.segments[index].locality != AdjustmentLocality::pixel_local) {
            return std::nullopt;
        }
    }
    const auto gaussian = [dimensions, scale_x, scale_y](const double sigma) {
        const double sigma_x = sigma * scale_x;
        const double sigma_y = sigma * scale_y;
        return WarmGaussianParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .horizontal_radius = static_cast<std::uint32_t>(std::ceil(3.0 * sigma_x)),
            .vertical_radius = static_cast<std::uint32_t>(std::ceil(3.0 * sigma_y)),
            .sigma_x = static_cast<float>(sigma_x),
            .sigma_y = static_cast<float>(sigma_y),
        };
    };
    const WarmGaussianParameters texture = gaussian(1.4);
    const WarmGaussianParameters small = gaussian(2.4);
    const WarmGaussianParameters large = gaussian(12.0);
    if (texture.horizontal_radius > 15U || texture.vertical_radius > 15U
        || small.horizontal_radius > 15U || small.vertical_radius > 15U
        || large.horizontal_radius > 15U || large.vertical_radius > 15U) {
        return std::nullopt;
    }
    WarmTextureClarityStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .post_nodes = std::vector<AdjustmentNode>(nodes.begin(), nodes.end()),
        .texture_gaussian = texture,
        .clarity_small_gaussian = small,
        .clarity_large_gaussian = large,
        .parameters = WarmTextureClarityParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .vertical_radius = large.vertical_radius,
            .sigma_y = large.sigma_y,
            .texture_amount = static_cast<float>(detail->texture),
            .clarity_amount = static_cast<float>(detail->clarity),
        },
    };
    auto& post_detail = std::get<SharpenAdjustment>(result.post_nodes[step.node_index].parameters);
    post_detail.texture = 0.0;
    post_detail.clarity = 0.0;
    result.before.segments.insert(
        result.before.segments.end(), plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    result.after.segments.push_back(EditExecutionSegment{
        .locality = AdjustmentLocality::pixel_local,
        .first_node_index = step.node_index,
        .past_last_node_index = step.node_index + 1U,
        .steps = {EditExecutionStep{.node_index = step.node_index,
                                    .operation = AdjustmentOperation::sharpen}},
    });
    const auto post_plan = compile_edit_execution_plan(result.post_nodes, scale_x, scale_y);
    for (const auto& post_segment : post_plan.segments) {
        EditExecutionSegment retained{
            .locality = post_segment.locality,
            .first_node_index = post_segment.first_node_index,
            .past_last_node_index = post_segment.past_last_node_index,
        };
        for (const auto& post_step : post_segment.steps) {
            if (post_step.node_index > step.node_index) {
                retained.steps.push_back(post_step);
            }
        }
        if (!retained.steps.empty()) {
            retained.first_node_index = retained.steps.front().node_index;
            retained.past_last_node_index = retained.steps.back().node_index + 1U;
            result.after.segments.push_back(std::move(retained));
        }
    }
    return result;
}

[[nodiscard]] std::optional<WarmLocalContrastStage> prepare_warm_local_contrast_stage(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const Dimensions dimensions,
    const double scale_x,
    const double scale_y
) {
    std::optional<std::size_t> neighbourhood_segment;
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (plan.segments[index].locality == AdjustmentLocality::neighborhood) {
            if (neighbourhood_segment.has_value()) {
                return std::nullopt;
            }
            neighbourhood_segment = index;
        }
    }
    if (!neighbourhood_segment.has_value()) {
        return std::nullopt;
    }
    const EditExecutionSegment& segment = plan.segments[*neighbourhood_segment];
    if (segment.steps.size() != 1U) {
        return std::nullopt;
    }
    const EditExecutionStep& step = segment.steps.front();
    if (step.operation != AdjustmentOperation::sharpen || step.node_index >= nodes.size()) {
        return std::nullopt;
    }
    const auto* detail = std::get_if<SharpenAdjustment>(&nodes[step.node_index].parameters);
    if (detail == nullptr
        || detail->execution_pass != DetailEffectsExecutionPass::color_grading
        || detail->local_contrast == 0.0 || detail->texture != 0.0
        || detail->clarity != 0.0) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (index != *neighbourhood_segment
            && plan.segments[index].locality != AdjustmentLocality::pixel_local) {
            return std::nullopt;
        }
    }
    const double effective_raster_scale = std::sqrt(std::max(0.0, scale_x * scale_y));
    const double large_radius = std::ceil(
        (20.0 + 60.0 * detail->local_contrast_scale) * effective_raster_scale
    );
    // The exact CPU algorithm is O(pixels) thanks to rolling means. A GPU box filter uses a
    // deliberately fixed loop, so only compact preview supports are admitted here. Larger
    // rasters preserve the exact same result by taking the normal CPU fallback path.
    if (large_radius > 32.0) {
        return std::nullopt;
    }
    const auto large_radius_u32 = static_cast<std::uint32_t>(std::max(1.0, large_radius));
    const auto small_radius_u32 = std::max(
        1U,
        static_cast<std::uint32_t>(std::ceil(static_cast<double>(large_radius_u32) * 0.32))
    );
    WarmLocalContrastStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .post_nodes = std::vector<AdjustmentNode>(nodes.begin(), nodes.end()),
        .small_box = WarmBoxParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .radius = small_radius_u32,
        },
        .large_box = WarmBoxParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .radius = large_radius_u32,
        },
        .small_coefficients = WarmGuidedCoefficientsParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .epsilon = 8.0e-4F,
        },
        .large_coefficients = WarmGuidedCoefficientsParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .epsilon = 1.6e-3F,
        },
        .parameters = WarmLocalContrastParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .amount = static_cast<float>(detail->local_contrast),
        },
    };
    auto& post_detail = std::get<SharpenAdjustment>(
        result.post_nodes[step.node_index].parameters
    );
    post_detail.local_contrast = 0.0;
    result.before.segments.insert(
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    // The zeroed node retains wheels in CPU order: local lightness contrast first, color wheels
    // next, then the remaining pixel-local recipe segments.
    result.after.segments.push_back(EditExecutionSegment{
        .locality = AdjustmentLocality::pixel_local,
        .first_node_index = step.node_index,
        .past_last_node_index = step.node_index + 1U,
        .steps = {EditExecutionStep{
            .node_index = step.node_index,
            .operation = AdjustmentOperation::sharpen,
        }},
    });
    const EditExecutionPlan post_plan = compile_edit_execution_plan(
        result.post_nodes,
        scale_x,
        scale_y
    );
    for (const auto& post_segment : post_plan.segments) {
        EditExecutionSegment retained{
            .locality = post_segment.locality,
            .first_node_index = post_segment.first_node_index,
            .past_last_node_index = post_segment.past_last_node_index,
        };
        for (const auto& post_step : post_segment.steps) {
            if (post_step.node_index > step.node_index) {
                retained.steps.push_back(post_step);
            }
        }
        if (!retained.steps.empty()) {
            retained.first_node_index = retained.steps.front().node_index;
            retained.past_last_node_index = retained.steps.back().node_index + 1U;
            result.after.segments.push_back(std::move(retained));
        }
    }
    return result;
}

[[nodiscard]] std::optional<WarmDehazeDefringeStage> prepare_warm_dehaze_defringe_stage(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const WorkingRgbSpace& working_space
) {
    std::optional<std::size_t> neighbourhood_segment;
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (plan.segments[index].locality == AdjustmentLocality::neighborhood) {
            if (neighbourhood_segment.has_value()) {
                return std::nullopt;
            }
            neighbourhood_segment = index;
        }
    }
    if (!neighbourhood_segment.has_value()) {
        return std::nullopt;
    }
    const EditExecutionSegment& segment = plan.segments[*neighbourhood_segment];
    if (segment.steps.size() != 1U) {
        return std::nullopt;
    }
    const EditExecutionStep& step = segment.steps.front();
    if (step.operation != AdjustmentOperation::sharpen || step.node_index >= nodes.size()) {
        return std::nullopt;
    }
    const auto* detail = std::get_if<SharpenAdjustment>(&nodes[step.node_index].parameters);
    if (detail == nullptr
        || detail->execution_pass != DetailEffectsExecutionPass::technical_detail
        || (detail->dehaze == 0.0 && detail->defringe_purple_amount == 0.0
            && detail->defringe_green_amount == 0.0)
        || detail->denoise_luminance != 0.0 || detail->denoise_color != 0.0
        || detail->amount != 0.0 || working_space.luminance_coefficients[1] <= 0.0) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (index != *neighbourhood_segment
            && plan.segments[index].locality != AdjustmentLocality::pixel_local) {
            return std::nullopt;
        }
    }

    WarmDehazeDefringeStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .post_nodes = std::vector<AdjustmentNode>(nodes.begin(), nodes.end()),
        .parameters = WarmDehazeDefringeParameters{
            .dehaze = static_cast<float>(detail->dehaze),
            .purple_amount = static_cast<float>(detail->defringe_purple_amount),
            .green_amount = static_cast<float>(detail->defringe_green_amount),
            .purple_hue_low = static_cast<float>(detail->defringe_purple_hue_low),
            .purple_hue_high = static_cast<float>(detail->defringe_purple_hue_high),
            .green_hue_low = static_cast<float>(detail->defringe_green_hue_low),
            .green_hue_high = static_cast<float>(detail->defringe_green_hue_high),
            .red_luminance = static_cast<float>(working_space.luminance_coefficients[0]),
            .green_luminance = static_cast<float>(working_space.luminance_coefficients[1]),
            .blue_luminance = static_cast<float>(working_space.luminance_coefficients[2]),
        },
    };
    auto& post_node = result.post_nodes[step.node_index];
    auto& post_detail = std::get<SharpenAdjustment>(post_node.parameters);
    // The Metal generic interpreter accepts only the color-grading variant of this shared node.
    // Turn it into an explicit no-op solely to obtain its working-space transform for the
    // technical optical kernel; all creative wheel controls are neutralized.
    post_node.implementation_version = color_grading_implementation_version;
    post_detail.execution_pass = DetailEffectsExecutionPass::color_grading;
    post_detail.dehaze = 0.0;
    post_detail.defringe_purple_amount = 0.0;
    post_detail.defringe_green_amount = 0.0;
    post_detail.clarity = 0.0;
    post_detail.texture = 0.0;
    post_detail.shadows_saturation = 0.0;
    post_detail.shadows_luminance = 0.0;
    post_detail.midtones_saturation = 0.0;
    post_detail.midtones_luminance = 0.0;
    post_detail.highlights_saturation = 0.0;
    post_detail.highlights_luminance = 0.0;

    result.before.segments.insert(
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    result.after.segments.push_back(EditExecutionSegment{
        .locality = AdjustmentLocality::pixel_local,
        .first_node_index = step.node_index,
        .past_last_node_index = step.node_index + 1U,
        .steps = {EditExecutionStep{
            .node_index = step.node_index,
            .operation = AdjustmentOperation::sharpen,
        }},
    });
    const EditExecutionPlan post_plan = compile_edit_execution_plan(result.post_nodes);
    for (const auto& post_segment : post_plan.segments) {
        EditExecutionSegment retained{
            .locality = post_segment.locality,
            .first_node_index = post_segment.first_node_index,
            .past_last_node_index = post_segment.past_last_node_index,
        };
        for (const auto& post_step : post_segment.steps) {
            if (post_step.node_index > step.node_index) {
                retained.steps.push_back(post_step);
            }
        }
        if (!retained.steps.empty()) {
            retained.first_node_index = retained.steps.front().node_index;
            retained.past_last_node_index = retained.steps.back().node_index + 1U;
            result.after.segments.push_back(std::move(retained));
        }
    }
    return result;
}

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

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

class WarmMetalContext final {
public:
    WarmMetalContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                diagnostic_ = "Metal could not create a warm-preview command queue";
                return;
            }

            MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
            options.mathMode = MTLMathModeSafe;
            NSError* error = nil;
            const std::string metal_source =
                make_metal_adjustment_source(warm_kernel_source);
            NSString* source = [[NSString alloc]
                initWithBytes:metal_source.data()
                       length:metal_source.size()
                     encoding:NSUTF8StringEncoding];
            if (source == nil) {
                [options release];
                diagnostic_ = "Metal warm-preview shader source is not valid UTF-8";
                return;
            }
            id<MTLLibrary> library =
                [device_ newLibraryWithSource:source options:options error:&error];
            [source release];
            [options release];
            if (library == nil) {
                diagnostic_ = "Metal warm-preview shader compilation failed: "
                    + error_description(error);
                return;
            }
            id<MTLFunction> display_function =
                [library newFunctionWithName:@"render_warm_preview_v1"];
            id<MTLFunction> adjustment_function =
                [library newFunctionWithName:@"execute_warm_adjustment_v1"];
            id<MTLFunction> denoise_function =
                [library newFunctionWithName:@"guided_denoise_warm_v1"];
            id<MTLFunction> sharpen_log_function =
                [library newFunctionWithName:@"warm_sharpen_log_luminance_v1"];
            id<MTLFunction> sharpen_horizontal_function =
                [library newFunctionWithName:@"warm_sharpen_horizontal_v1"];
            id<MTLFunction> sharpen_apply_function =
                [library newFunctionWithName:@"warm_sharpen_apply_v1"];
            id<MTLFunction> texture_lightness_function =
                [library newFunctionWithName:@"warm_texture_lightness_v1"];
            id<MTLFunction> texture_horizontal_function =
                [library newFunctionWithName:@"warm_texture_horizontal_v1"];
            id<MTLFunction> texture_apply_function =
                [library newFunctionWithName:@"warm_texture_apply_v1"];
            id<MTLFunction> scalar_vertical_function =
                [library newFunctionWithName:@"warm_scalar_vertical_v1"];
            id<MTLFunction> clarity_apply_function =
                [library newFunctionWithName:@"warm_clarity_apply_v1"];
            if (display_function == nil || adjustment_function == nil
                || denoise_function == nil || sharpen_log_function == nil
                || sharpen_horizontal_function == nil || sharpen_apply_function == nil
                || texture_lightness_function == nil || texture_horizontal_function == nil
                || texture_apply_function == nil || scalar_vertical_function == nil
                || clarity_apply_function == nil) {
                [display_function release];
                [adjustment_function release];
                [denoise_function release];
                [sharpen_log_function release];
                [sharpen_horizontal_function release];
                [sharpen_apply_function release];
                [texture_lightness_function release];
                [texture_horizontal_function release];
                [texture_apply_function release];
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview shader entry point is unavailable";
                return;
            }
            display_pipeline_ = [device_
                newComputePipelineStateWithFunction:display_function error:&error];
            [display_function release];
            if (display_pipeline_ == nil) {
                [adjustment_function release];
                [denoise_function release];
                [sharpen_log_function release];
                [sharpen_horizontal_function release];
                [sharpen_apply_function release];
                [texture_lightness_function release];
                [texture_horizontal_function release];
                [texture_apply_function release];
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview pipeline creation failed: "
                    + error_description(error);
                return;
            }
            adjustment_pipeline_ = [device_
                newComputePipelineStateWithFunction:adjustment_function error:&error];
            [adjustment_function release];
            if (adjustment_pipeline_ == nil) {
                [denoise_function release];
                [sharpen_log_function release];
                [sharpen_horizontal_function release];
                [sharpen_apply_function release];
                [texture_lightness_function release];
                [texture_horizontal_function release];
                [texture_apply_function release];
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview adjustment pipeline creation failed: "
                    + error_description(error);
                return;
            }
            denoise_pipeline_ = [device_
                newComputePipelineStateWithFunction:denoise_function error:&error];
            [denoise_function release];
            if (denoise_pipeline_ == nil) {
                [sharpen_log_function release];
                [sharpen_horizontal_function release];
                [sharpen_apply_function release];
                [texture_lightness_function release];
                [texture_horizontal_function release];
                [texture_apply_function release];
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview denoise pipeline creation failed: "
                    + error_description(error);
                return;
            }
            sharpen_log_pipeline_ = [device_
                newComputePipelineStateWithFunction:sharpen_log_function error:&error];
            [sharpen_log_function release];
            if (sharpen_log_pipeline_ == nil) {
                [sharpen_horizontal_function release];
                [sharpen_apply_function release];
                [texture_lightness_function release];
                [texture_horizontal_function release];
                [texture_apply_function release];
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview sharpen-log pipeline creation failed: "
                    + error_description(error);
                return;
            }
            sharpen_horizontal_pipeline_ = [device_
                newComputePipelineStateWithFunction:sharpen_horizontal_function error:&error];
            [sharpen_horizontal_function release];
            if (sharpen_horizontal_pipeline_ == nil) {
                [sharpen_apply_function release];
                [texture_lightness_function release];
                [texture_horizontal_function release];
                [texture_apply_function release];
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview sharpen-horizontal pipeline creation failed: "
                    + error_description(error);
                return;
            }
            sharpen_apply_pipeline_ = [device_
                newComputePipelineStateWithFunction:sharpen_apply_function error:&error];
            [sharpen_apply_function release];
            if (sharpen_apply_pipeline_ == nil) {
                [texture_lightness_function release];
                [texture_horizontal_function release];
                [texture_apply_function release];
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview sharpen-apply pipeline creation failed: "
                    + error_description(error);
                return;
            }
            texture_lightness_pipeline_ = [device_
                newComputePipelineStateWithFunction:texture_lightness_function error:&error];
            [texture_lightness_function release];
            if (texture_lightness_pipeline_ == nil) {
                [texture_horizontal_function release];
                [texture_apply_function release];
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview texture-lightness pipeline creation failed: "
                    + error_description(error);
                return;
            }
            texture_horizontal_pipeline_ = [device_
                newComputePipelineStateWithFunction:texture_horizontal_function error:&error];
            [texture_horizontal_function release];
            if (texture_horizontal_pipeline_ == nil) {
                [texture_apply_function release];
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview texture-horizontal pipeline creation failed: "
                    + error_description(error);
                return;
            }
            texture_apply_pipeline_ = [device_
                newComputePipelineStateWithFunction:texture_apply_function error:&error];
            [texture_apply_function release];
            if (texture_apply_pipeline_ == nil) {
                [scalar_vertical_function release];
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview texture-apply pipeline creation failed: "
                    + error_description(error);
                return;
            }
            scalar_vertical_pipeline_ = [device_
                newComputePipelineStateWithFunction:scalar_vertical_function error:&error];
            [scalar_vertical_function release];
            if (scalar_vertical_pipeline_ == nil) {
                [clarity_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview scalar-vertical pipeline creation failed: "
                    + error_description(error);
                return;
            }
            clarity_apply_pipeline_ = [device_
                newComputePipelineStateWithFunction:clarity_apply_function error:&error];
            [clarity_apply_function release];
            if (clarity_apply_pipeline_ == nil) {
                [library release];
                diagnostic_ = "Metal warm-preview clarity-apply pipeline creation failed: "
                    + error_description(error);
                return;
            }
            id<MTLFunction> dehaze_defringe_function =
                [library newFunctionWithName:@"warm_dehaze_defringe_v1"];
            if (dehaze_defringe_function == nil) {
                [library release];
                diagnostic_ = "Metal warm-preview dehaze-defringe shader entry point is unavailable";
                return;
            }
            dehaze_defringe_pipeline_ = [device_
                newComputePipelineStateWithFunction:dehaze_defringe_function error:&error];
            [dehaze_defringe_function release];
            if (dehaze_defringe_pipeline_ == nil) {
                [library release];
                diagnostic_ = "Metal warm-preview dehaze-defringe pipeline creation failed: "
                    + error_description(error);
                return;
            }
            id<MTLFunction> texture_clarity_apply_function =
                [library newFunctionWithName:@"warm_texture_clarity_apply_v1"];
            if (texture_clarity_apply_function == nil) {
                [library release];
                diagnostic_ = "Metal warm-preview texture-clarity shader entry point is unavailable";
                return;
            }
            texture_clarity_apply_pipeline_ = [device_
                newComputePipelineStateWithFunction:texture_clarity_apply_function error:&error];
            [texture_clarity_apply_function release];
            if (texture_clarity_apply_pipeline_ == nil) {
                [library release];
                diagnostic_ = "Metal warm-preview texture-clarity pipeline creation failed: "
                    + error_description(error);
                return;
            }
            id<MTLFunction> box_horizontal_function =
                [library newFunctionWithName:@"warm_box_horizontal_v1"];
            id<MTLFunction> box_vertical_function =
                [library newFunctionWithName:@"warm_box_vertical_v1"];
            id<MTLFunction> scalar_square_function =
                [library newFunctionWithName:@"warm_scalar_square_v1"];
            id<MTLFunction> guided_coefficients_function =
                [library newFunctionWithName:@"warm_guided_coefficients_v1"];
            id<MTLFunction> guided_combine_function =
                [library newFunctionWithName:@"warm_guided_combine_v1"];
            id<MTLFunction> local_contrast_apply_function =
                [library newFunctionWithName:@"warm_local_contrast_apply_v1"];
            if (box_horizontal_function == nil || box_vertical_function == nil
                || scalar_square_function == nil || guided_coefficients_function == nil
                || guided_combine_function == nil || local_contrast_apply_function == nil) {
                [box_horizontal_function release];
                [box_vertical_function release];
                [scalar_square_function release];
                [guided_coefficients_function release];
                [guided_combine_function release];
                [local_contrast_apply_function release];
                [library release];
                diagnostic_ = "Metal warm-preview local-contrast shader entry point is unavailable";
                return;
            }
            box_horizontal_pipeline_ = [device_
                newComputePipelineStateWithFunction:box_horizontal_function error:&error];
            [box_horizontal_function release];
            box_vertical_pipeline_ = [device_
                newComputePipelineStateWithFunction:box_vertical_function error:&error];
            [box_vertical_function release];
            scalar_square_pipeline_ = [device_
                newComputePipelineStateWithFunction:scalar_square_function error:&error];
            [scalar_square_function release];
            guided_coefficients_pipeline_ = [device_
                newComputePipelineStateWithFunction:guided_coefficients_function error:&error];
            [guided_coefficients_function release];
            guided_combine_pipeline_ = [device_
                newComputePipelineStateWithFunction:guided_combine_function error:&error];
            [guided_combine_function release];
            local_contrast_apply_pipeline_ = [device_
                newComputePipelineStateWithFunction:local_contrast_apply_function error:&error];
            [local_contrast_apply_function release];
            [library release];
            if (box_horizontal_pipeline_ == nil || box_vertical_pipeline_ == nil
                || scalar_square_pipeline_ == nil || guided_coefficients_pipeline_ == nil
                || guided_combine_pipeline_ == nil || local_contrast_apply_pipeline_ == nil) {
                diagnostic_ = "Metal warm-preview local-contrast pipeline creation failed: "
                    + error_description(error);
            }
        }
    }

    ~WarmMetalContext() {
        [local_contrast_apply_pipeline_ release];
        [guided_combine_pipeline_ release];
        [guided_coefficients_pipeline_ release];
        [scalar_square_pipeline_ release];
        [box_vertical_pipeline_ release];
        [box_horizontal_pipeline_ release];
        [texture_clarity_apply_pipeline_ release];
        [dehaze_defringe_pipeline_ release];
        [clarity_apply_pipeline_ release];
        [scalar_vertical_pipeline_ release];
        [texture_apply_pipeline_ release];
        [texture_horizontal_pipeline_ release];
        [texture_lightness_pipeline_ release];
        [sharpen_apply_pipeline_ release];
        [sharpen_horizontal_pipeline_ release];
        [sharpen_log_pipeline_ release];
        [denoise_pipeline_ release];
        [adjustment_pipeline_ release];
        [display_pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    WarmMetalContext(const WarmMetalContext&) = delete;
    WarmMetalContext& operator=(const WarmMetalContext&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return device_ != nil && queue_ != nil && display_pipeline_ != nil
            && adjustment_pipeline_ != nil && denoise_pipeline_ != nil
            && sharpen_log_pipeline_ != nil && sharpen_horizontal_pipeline_ != nil
            && sharpen_apply_pipeline_ != nil && texture_lightness_pipeline_ != nil
            && texture_horizontal_pipeline_ != nil && texture_apply_pipeline_ != nil
            && scalar_vertical_pipeline_ != nil && clarity_apply_pipeline_ != nil
            && dehaze_defringe_pipeline_ != nil && texture_clarity_apply_pipeline_ != nil
            && box_horizontal_pipeline_ != nil && box_vertical_pipeline_ != nil
            && scalar_square_pipeline_ != nil && guided_coefficients_pipeline_ != nil
            && guided_combine_pipeline_ != nil && local_contrast_apply_pipeline_ != nil;
    }
    [[nodiscard]] id<MTLDevice> device() const noexcept { return device_; }
    [[nodiscard]] id<MTLCommandQueue> queue() const noexcept { return queue_; }
    [[nodiscard]] id<MTLComputePipelineState> display_pipeline() const noexcept {
        return display_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> adjustment_pipeline() const noexcept {
        return adjustment_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> denoise_pipeline() const noexcept {
        return denoise_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> sharpen_log_pipeline() const noexcept {
        return sharpen_log_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> sharpen_horizontal_pipeline() const noexcept {
        return sharpen_horizontal_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> sharpen_apply_pipeline() const noexcept {
        return sharpen_apply_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> texture_lightness_pipeline() const noexcept {
        return texture_lightness_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> texture_horizontal_pipeline() const noexcept {
        return texture_horizontal_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> texture_apply_pipeline() const noexcept {
        return texture_apply_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> scalar_vertical_pipeline() const noexcept {
        return scalar_vertical_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> clarity_apply_pipeline() const noexcept {
        return clarity_apply_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> dehaze_defringe_pipeline() const noexcept {
        return dehaze_defringe_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> texture_clarity_apply_pipeline() const noexcept {
        return texture_clarity_apply_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> box_horizontal_pipeline() const noexcept {
        return box_horizontal_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> box_vertical_pipeline() const noexcept {
        return box_vertical_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> scalar_square_pipeline() const noexcept {
        return scalar_square_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> guided_coefficients_pipeline() const noexcept {
        return guided_coefficients_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> guided_combine_pipeline() const noexcept {
        return guided_combine_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> local_contrast_apply_pipeline() const noexcept {
        return local_contrast_apply_pipeline_;
    }
    [[nodiscard]] const std::string& diagnostic() const noexcept {
        return diagnostic_;
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> display_pipeline_ = nil;
    id<MTLComputePipelineState> adjustment_pipeline_ = nil;
    id<MTLComputePipelineState> denoise_pipeline_ = nil;
    id<MTLComputePipelineState> sharpen_log_pipeline_ = nil;
    id<MTLComputePipelineState> sharpen_horizontal_pipeline_ = nil;
    id<MTLComputePipelineState> sharpen_apply_pipeline_ = nil;
    id<MTLComputePipelineState> texture_lightness_pipeline_ = nil;
    id<MTLComputePipelineState> texture_horizontal_pipeline_ = nil;
    id<MTLComputePipelineState> texture_apply_pipeline_ = nil;
    id<MTLComputePipelineState> scalar_vertical_pipeline_ = nil;
    id<MTLComputePipelineState> clarity_apply_pipeline_ = nil;
    id<MTLComputePipelineState> dehaze_defringe_pipeline_ = nil;
    id<MTLComputePipelineState> texture_clarity_apply_pipeline_ = nil;
    id<MTLComputePipelineState> box_horizontal_pipeline_ = nil;
    id<MTLComputePipelineState> box_vertical_pipeline_ = nil;
    id<MTLComputePipelineState> scalar_square_pipeline_ = nil;
    id<MTLComputePipelineState> guided_coefficients_pipeline_ = nil;
    id<MTLComputePipelineState> guided_combine_pipeline_ = nil;
    id<MTLComputePipelineState> local_contrast_apply_pipeline_ = nil;
    std::string diagnostic_;
};

[[nodiscard]] WarmMetalContext& metal_context() {
    // Device, queue and immutable pipeline state are process-wide. MTLCommandQueue is safe for
    // concurrent command-buffer creation; mutable raster resources remain session/slot-local.
    static WarmMetalContext context;
    return context;
}

[[nodiscard]] bool force_test_failure() noexcept {
    const char* configured = std::getenv("SHADOW_TEST_WARM_METAL_FORCE_FAILURE");
    return configured != nullptr && std::string_view(configured) == "1";
}

[[nodiscard]] std::string command_buffer_diagnostic(
    id<MTLCommandBuffer> command_buffer
) {
    const std::string detail = error_description(command_buffer.error);
    return detail.empty()
        ? "Metal warm-preview command did not complete successfully"
        : "Metal warm-preview command failed: " + detail;
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
    const auto technical_detail_stage = prepare_warm_technical_detail_stage(
        nodes,
        plan,
        impl_->dimensions,
        impl_->working_space,
        impl_->level_zero_to_raster_scale_x,
        impl_->level_zero_to_raster_scale_y
    );
    const auto texture_clarity_stage = technical_detail_stage.has_value()
        ? std::optional<WarmTextureClarityStage>{}
        : prepare_warm_texture_clarity_stage(
            nodes,
            plan,
            impl_->dimensions,
            impl_->level_zero_to_raster_scale_x,
            impl_->level_zero_to_raster_scale_y
        );
    const auto local_contrast_stage = technical_detail_stage.has_value()
            || texture_clarity_stage.has_value()
        ? std::optional<WarmLocalContrastStage>{}
        : prepare_warm_local_contrast_stage(
            nodes,
            plan,
            impl_->dimensions,
            impl_->level_zero_to_raster_scale_x,
            impl_->level_zero_to_raster_scale_y
        );
    const auto texture_stage = technical_detail_stage.has_value() || texture_clarity_stage.has_value()
            || local_contrast_stage.has_value()
        ? std::optional<WarmTextureStage>{}
        : prepare_warm_texture_stage(
            nodes,
            plan,
            impl_->dimensions,
            impl_->level_zero_to_raster_scale_x,
            impl_->level_zero_to_raster_scale_y
        );
    const auto clarity_stage = technical_detail_stage.has_value() || texture_clarity_stage.has_value()
            || local_contrast_stage.has_value() || texture_stage.has_value()
        ? std::optional<WarmClarityStage>{}
        : prepare_warm_clarity_stage(
            nodes,
            plan,
            impl_->dimensions,
            impl_->level_zero_to_raster_scale_x,
            impl_->level_zero_to_raster_scale_y
        );
    const auto dehaze_defringe_stage = technical_detail_stage.has_value()
            || texture_clarity_stage.has_value() || local_contrast_stage.has_value()
            || texture_stage.has_value() || clarity_stage.has_value()
        ? std::optional<WarmDehazeDefringeStage>{}
        : prepare_warm_dehaze_defringe_stage(nodes, plan, impl_->working_space);
    const bool has_neighbourhood_stage = technical_detail_stage.has_value()
        || texture_clarity_stage.has_value() || local_contrast_stage.has_value()
        || texture_stage.has_value() || clarity_stage.has_value()
        || dehaze_defringe_stage.has_value();
    PreparedMetalAdjustment before_program;
    std::optional<PreparedMetalAdjustment> final_program;
    if (technical_detail_stage.has_value()) {
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
    } else if (texture_clarity_stage.has_value()) {
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
    } else if (local_contrast_stage.has_value()) {
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
    } else if (texture_stage.has_value()) {
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
    } else if (clarity_stage.has_value()) {
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
    } else if (dehaze_defringe_stage.has_value()) {
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
    if (technical_detail_stage.has_value()) {
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
    } else if (texture_clarity_stage.has_value()) {
        const std::string diagnostic = impl_->ensure_texture_clarity_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = diagnostic};
        }
    } else if (local_contrast_stage.has_value()) {
        const std::string diagnostic = impl_->ensure_local_contrast_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{.status = RenderStatus::unavailable_or_failed,
                                 .output = std::nullopt,
                                 .diagnostic = diagnostic};
        }
    } else if (texture_stage.has_value()) {
        const std::string diagnostic = impl_->ensure_sharpen_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    } else if (clarity_stage.has_value()) {
        const std::string diagnostic = impl_->ensure_clarity_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    } else if (dehaze_defringe_stage.has_value()) {
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
        if (technical_detail_stage.has_value()) {
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
        } else if (texture_clarity_stage.has_value()) {
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
        } else if (local_contrast_stage.has_value()) {
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
        } else if (texture_stage.has_value()) {
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
        } else if (clarity_stage.has_value()) {
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
        } else if (dehaze_defringe_stage.has_value()) {
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
