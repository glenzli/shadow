#include "warm_edit_gpu_neighbourhood_plan.hpp"

#include "../edit/guided_selective_tone.hpp"
#include "../edit/working_color_math.hpp"

#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace shadow::image::detail {

namespace {

[[nodiscard]] bool
is_gpu_warm_technical_detail_supported(const SharpenAdjustment& parameters) noexcept {
    return parameters.execution_pass == DetailEffectsExecutionPass::technical_detail
           && (parameters.denoise_luminance > 0.0 || parameters.denoise_color > 0.0
               || parameters.amount > 0.0 || parameters.dehaze != 0.0
               || parameters.defringe_purple_amount > 0.0
               || parameters.defringe_green_amount > 0.0);
}

[[nodiscard]] bool fill_selective_tone_matrix_rows(
    const Matrix3& matrix,
    std::array<float, 4U>& row_0,
    std::array<float, 4U>& row_1,
    std::array<float, 4U>& row_2
) noexcept {
    std::array<std::array<float, 4U>*, 3U> rows{&row_0, &row_1, &row_2};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            const float converted = static_cast<float>(matrix[row][column]);
            if (!std::isfinite(converted)) {
                return false;
            }
            (*rows[row])[column] = converted;
        }
    }
    return true;
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

    const double strength = std::max(detail->denoise_luminance, detail->denoise_color);
    const double authority =
        std::clamp(strength * strength * (1.0 - 0.60 * detail->denoise_detail), 0.0, 1.0);
    WarmTechnicalDetailStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
    };
    if (detail->denoise_luminance > 0.0 || detail->denoise_color > 0.0) {
        result.denoise = WarmDenoiseParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .radius =
                static_cast<std::uint32_t>(std::clamp(std::ceil(1.0 + 3.0 * authority), 1.0, 4.0)),
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
    if (detail->dehaze != 0.0 || detail->defringe_purple_amount > 0.0
        || detail->defringe_green_amount > 0.0) {
        result.dehaze_defringe = WarmDehazeDefringeParameters{
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
        };
        result.post_nodes = std::vector<AdjustmentNode>(nodes.begin(), nodes.end());
        AdjustmentNode& post_node = result.post_nodes[step.node_index];
        post_node.implementation_version = color_grading_implementation_version;
        post_node.parameters = SharpenAdjustment{
            .execution_pass = DetailEffectsExecutionPass::color_grading,
        };
        result.after.segments.push_back(
            EditExecutionSegment{
                .locality = AdjustmentLocality::pixel_local,
                .first_node_index = step.node_index,
                .past_last_node_index = step.node_index + 1U,
                .steps = {EditExecutionStep{
                    .node_index = step.node_index,
                    .operation = AdjustmentOperation::sharpen,
                }},
            }
        );
    }
    if (detail->amount > 0.0) {
        const double radius_x = std::ceil(3.0 * detail->radius * level_zero_to_raster_scale_x);
        const double radius_y = std::ceil(3.0 * detail->radius * level_zero_to_raster_scale_y);
        // Keep capture sharpening compact even though the shader loop is dynamic. A source that
        // needs wider support declines and replays through the complete CPU oracle.
        if (radius_x > static_cast<double>(warm_sharpen_radius_limit)
            || radius_y > static_cast<double>(warm_sharpen_radius_limit)) {
            return std::nullopt;
        }
        result.sharpen = WarmSharpenParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .horizontal_radius = static_cast<std::uint32_t>(radius_x),
            .vertical_radius = static_cast<std::uint32_t>(radius_y),
            .sigma_x = static_cast<float>(detail->radius * level_zero_to_raster_scale_x),
            .sigma_y = static_cast<float>(detail->radius * level_zero_to_raster_scale_y),
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
    if (detail == nullptr || detail->execution_pass != DetailEffectsExecutionPass::color_grading
        || detail->texture == 0.0 || detail->clarity != 0.0 || detail->local_contrast != 0.0) {
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
    if (radius_x > static_cast<double>(warm_creative_gaussian_radius_limit)
        || radius_y > static_cast<double>(warm_creative_gaussian_radius_limit)) {
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
    auto& post_detail = std::get<SharpenAdjustment>(result.post_nodes[step.node_index].parameters);
    post_detail.texture = 0.0;

    result.before.segments.insert(
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    // Recompile only the post portion. Keep one explicit replacement step even when all wheels
    // are neutral: it obtains the validated working-space transform for the Texture kernels and
    // is an exact no-op in the final pixel-local interpreter.
    result.after.segments.push_back(
        EditExecutionSegment{
            .locality = AdjustmentLocality::pixel_local,
            .first_node_index = step.node_index,
            .past_last_node_index = step.node_index + 1U,
            .steps = {EditExecutionStep{
                .node_index = step.node_index,
                .operation = AdjustmentOperation::sharpen,
            }},
        }
    );
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
    if (detail == nullptr || detail->execution_pass != DetailEffectsExecutionPass::color_grading
        || detail->clarity == 0.0 || detail->texture != 0.0 || detail->local_contrast != 0.0) {
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
    // Native Clarity needs a 36-pixel large band. Admit it explicitly while keeping unusually
    // enlarged rasters bounded so a malformed scale cannot create unbounded shader work.
    if (small_radius_x > static_cast<double>(warm_creative_gaussian_radius_limit)
        || small_radius_y > static_cast<double>(warm_creative_gaussian_radius_limit)
        || large_radius_x > static_cast<double>(warm_creative_gaussian_radius_limit)
        || large_radius_y > static_cast<double>(warm_creative_gaussian_radius_limit)) {
        return std::nullopt;
    }

    WarmClarityStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .post_nodes = std::vector<AdjustmentNode>(nodes.begin(), nodes.end()),
        .small_gaussian =
            WarmGaussianParameters{
                .width = dimensions.width,
                .height = dimensions.height,
                .horizontal_radius = static_cast<std::uint32_t>(small_radius_x),
                .vertical_radius = static_cast<std::uint32_t>(small_radius_y),
                .sigma_x = static_cast<float>(small_sigma_x),
                .sigma_y = static_cast<float>(small_sigma_y),
            },
        .large_gaussian =
            WarmGaussianParameters{
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
    auto& post_detail = std::get<SharpenAdjustment>(result.post_nodes[step.node_index].parameters);
    post_detail.clarity = 0.0;

    result.before.segments.insert(
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    // The zeroed replacement preserves the CPU ordering: Clarity first, color wheels in this
    // node second, then all subsequent pixel-local nodes.
    result.after.segments.push_back(
        EditExecutionSegment{
            .locality = AdjustmentLocality::pixel_local,
            .first_node_index = step.node_index,
            .past_last_node_index = step.node_index + 1U,
            .steps = {EditExecutionStep{
                .node_index = step.node_index,
                .operation = AdjustmentOperation::sharpen,
            }},
        }
    );
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
        || detail->texture == 0.0 || detail->clarity == 0.0 || detail->local_contrast != 0.0) {
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
    if (texture.horizontal_radius > warm_creative_gaussian_radius_limit
        || texture.vertical_radius > warm_creative_gaussian_radius_limit
        || small.horizontal_radius > warm_creative_gaussian_radius_limit
        || small.vertical_radius > warm_creative_gaussian_radius_limit
        || large.horizontal_radius > warm_creative_gaussian_radius_limit
        || large.vertical_radius > warm_creative_gaussian_radius_limit) {
        return std::nullopt;
    }
    WarmTextureClarityStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .post_nodes = std::vector<AdjustmentNode>(nodes.begin(), nodes.end()),
        .texture_gaussian = texture,
        .clarity_small_gaussian = small,
        .clarity_large_gaussian = large,
        .parameters = WarmCreativeDetailParameters{
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
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    result.after.segments.push_back(
        EditExecutionSegment{
            .locality = AdjustmentLocality::pixel_local,
            .first_node_index = step.node_index,
            .past_last_node_index = step.node_index + 1U,
            .steps = {EditExecutionStep{
                .node_index = step.node_index,
                .operation = AdjustmentOperation::sharpen
            }},
        }
    );
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
    if (detail == nullptr || detail->execution_pass != DetailEffectsExecutionPass::color_grading
        || detail->local_contrast == 0.0) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (index != *neighbourhood_segment
            && plan.segments[index].locality != AdjustmentLocality::pixel_local) {
            return std::nullopt;
        }
    }
    const double effective_raster_scale = std::sqrt(std::max(0.0, scale_x * scale_y));
    const double large_radius =
        std::ceil((20.0 + 60.0 * detail->local_contrast_scale) * effective_raster_scale);
    // Match the CPU rolling-window complexity on Metal. The authored scale is bounded to an
    // 80-pixel level-zero radius; reject only an inconsistent enlarged raster contract.
    if (large_radius > static_cast<double>(warm_local_contrast_box_radius_limit)) {
        return std::nullopt;
    }
    const auto large_radius_u32 = static_cast<std::uint32_t>(std::max(1.0, large_radius));
    const auto small_radius_u32 = std::max(
        1U,
        static_cast<std::uint32_t>(std::ceil(static_cast<double>(large_radius_u32) * 0.32))
    );
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
    const std::optional<WarmGaussianParameters> texture =
        detail->texture == 0.0 ? std::nullopt
                               : std::optional<WarmGaussianParameters>{gaussian(1.4)};
    const std::optional<WarmGaussianParameters> clarity_small =
        detail->clarity == 0.0 ? std::nullopt
                               : std::optional<WarmGaussianParameters>{gaussian(2.4)};
    const std::optional<WarmGaussianParameters> clarity_large =
        detail->clarity == 0.0 ? std::nullopt
                               : std::optional<WarmGaussianParameters>{gaussian(12.0)};
    const auto radius_supported = [](const std::optional<WarmGaussianParameters>& parameters) {
        return !parameters.has_value()
               || (parameters->horizontal_radius <= warm_creative_gaussian_radius_limit
                   && parameters->vertical_radius <= warm_creative_gaussian_radius_limit);
    };
    if (!radius_supported(texture) || !radius_supported(clarity_small)
        || !radius_supported(clarity_large)) {
        return std::nullopt;
    }
    WarmLocalContrastStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .post_nodes = std::vector<AdjustmentNode>(nodes.begin(), nodes.end()),
        .texture_gaussian = texture,
        .clarity_small_gaussian = clarity_small,
        .clarity_large_gaussian = clarity_large,
        .small_box =
            WarmBoxParameters{
                .width = dimensions.width,
                .height = dimensions.height,
                .radius = small_radius_u32,
            },
        .large_box =
            WarmBoxParameters{
                .width = dimensions.width,
                .height = dimensions.height,
                .radius = large_radius_u32,
            },
        .small_coefficients =
            WarmGuidedCoefficientsParameters{
                .width = dimensions.width,
                .height = dimensions.height,
                .epsilon = 8.0e-4F,
            },
        .large_coefficients =
            WarmGuidedCoefficientsParameters{
                .width = dimensions.width,
                .height = dimensions.height,
                .epsilon = 1.6e-3F,
            },
        .parameters = WarmCreativeDetailParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .vertical_radius = clarity_large.has_value() ? clarity_large->vertical_radius : 0U,
            .sigma_y = clarity_large.has_value() ? clarity_large->sigma_y : 1.0F,
            .texture_amount = static_cast<float>(detail->texture),
            .clarity_amount = static_cast<float>(detail->clarity),
            .local_contrast_amount = static_cast<float>(detail->local_contrast),
        },
    };
    auto& post_detail = std::get<SharpenAdjustment>(result.post_nodes[step.node_index].parameters);
    post_detail.texture = 0.0;
    post_detail.clarity = 0.0;
    post_detail.local_contrast = 0.0;
    result.before.segments.insert(
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    // The zeroed node retains wheels in CPU order: local lightness contrast first, color wheels
    // next, then the remaining pixel-local recipe segments.
    result.after.segments.push_back(
        EditExecutionSegment{
            .locality = AdjustmentLocality::pixel_local,
            .first_node_index = step.node_index,
            .past_last_node_index = step.node_index + 1U,
            .steps = {EditExecutionStep{
                .node_index = step.node_index,
                .operation = AdjustmentOperation::sharpen,
            }},
        }
    );
    const EditExecutionPlan post_plan =
        compile_edit_execution_plan(result.post_nodes, scale_x, scale_y);
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

[[nodiscard]] std::optional<WarmSelectiveToneStage> prepare_warm_selective_tone_stage(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const Dimensions dimensions,
    const WorkingRgbSpace& working_space,
    const double scale_x,
    const double scale_y
) {
    std::optional<std::size_t> neighbourhood_segment;
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (plan.segments[index].locality != AdjustmentLocality::neighborhood) {
            continue;
        }
        if (neighbourhood_segment.has_value()) {
            return std::nullopt;
        }
        neighbourhood_segment = index;
    }
    if (!neighbourhood_segment.has_value()) {
        return std::nullopt;
    }
    const EditExecutionSegment& segment = plan.segments[*neighbourhood_segment];
    if (segment.steps.size() != 1U) {
        return std::nullopt;
    }
    const EditExecutionStep& step = segment.steps.front();
    if (step.operation != AdjustmentOperation::selective_tone || step.node_index >= nodes.size()) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (index != *neighbourhood_segment
            && plan.segments[index].locality != AdjustmentLocality::pixel_local) {
            return std::nullopt;
        }
    }
    const auto& parameters = std::get<SelectiveToneAdjustment>(nodes[step.node_index].parameters);
    const PreparedGuidedSelectiveTone prepared =
        prepare_guided_selective_tone(parameters, scale_x, scale_y);
    constexpr std::uint32_t maximum_selective_tone_box_radius = 64U;
    if (prepared.neutral() || prepared.mask_radius_x() == 0U || prepared.mask_radius_y() == 0U
        || prepared.mask_radius_x() > maximum_selective_tone_box_radius
        || prepared.mask_radius_y() > maximum_selective_tone_box_radius) {
        return std::nullopt;
    }
    const AdjustmentNode& node = nodes[step.node_index];
    const WorkingSpaceTransform transform =
        prepare_working_space_transform(working_space, node, step.node_index);
    WarmSelectiveToneStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .horizontal_box =
            WarmBoxParameters{
                .width = dimensions.width,
                .height = dimensions.height,
                .radius = prepared.mask_radius_x(),
            },
        .vertical_box =
            WarmBoxParameters{
                .width = dimensions.width,
                .height = dimensions.height,
                .radius = prepared.mask_radius_y(),
            },
        .coefficients =
            WarmGuidedCoefficientsParameters{
                .width = dimensions.width,
                .height = dimensions.height,
                .epsilon = 0.12F * 0.12F,
            },
        .parameters = WarmSelectiveToneParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .highlights = static_cast<float>(prepared.highlights()),
            .shadows = static_cast<float>(prepared.shadows()),
            .whites = static_cast<float>(prepared.whites()),
            .blacks = static_cast<float>(prepared.blacks()),
            .red_luminance = static_cast<float>(working_space.luminance_coefficients[0]),
            .green_luminance = static_cast<float>(working_space.luminance_coefficients[1]),
            .blue_luminance = static_cast<float>(working_space.luminance_coefficients[2]),
        },
    };
    if (!fill_selective_tone_matrix_rows(
            transform.rgb_to_xyz,
            result.parameters.rgb_to_xyz_row_0,
            result.parameters.rgb_to_xyz_row_1,
            result.parameters.rgb_to_xyz_row_2
        )
        || !fill_selective_tone_matrix_rows(
            transform.xyz_to_rgb,
            result.parameters.xyz_to_rgb_row_0,
            result.parameters.xyz_to_rgb_row_1,
            result.parameters.xyz_to_rgb_row_2
        )) {
        return std::nullopt;
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

template <typename Stage>
[[nodiscard]] std::optional<WarmGpuNeighbourhoodStage>
isolate_stage_post_program(std::optional<Stage> stage) {
    if (!stage.has_value()) {
        return std::nullopt;
    }
    stage->before.segments.clear();
    if constexpr (requires(Stage value) { value.post_nodes; }) {
        if (stage->post_nodes.empty()) {
            stage->after.segments.clear();
        } else {
            if (stage->after.segments.empty()) {
                return std::nullopt;
            }
            stage->after.segments.erase(
                stage->after.segments.begin() + 1,
                stage->after.segments.end()
            );
        }
    } else {
        stage->after.segments.clear();
    }
    return WarmGpuNeighbourhoodStage{std::move(*stage)};
}

} // namespace

std::optional<WarmGpuNeighbourhoodStage> prepare_warm_gpu_neighbourhood_stage(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionStep& step,
    const Dimensions dimensions,
    const WorkingRgbSpace& working_space,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y,
    const AdjustmentExecutionContext context
) {
    if (step.node_index >= nodes.size()
        || operation(nodes[step.node_index].parameters) != step.operation) {
        return std::nullopt;
    }
    const EditExecutionPlan plan{
        .source_node_count = nodes.size(),
        .segments = {
            EditExecutionSegment{
                .locality = AdjustmentLocality::neighborhood,
                .first_node_index = step.node_index,
                .past_last_node_index = step.node_index + 1U,
                .steps = {step},
            },
        },
    };
    if (step.operation == AdjustmentOperation::spot_heal) {
        const auto* retouch = std::get_if<SpotHealAdjustment>(&nodes[step.node_index].parameters);
        if (retouch == nullptr) {
            return std::nullopt;
        }
        auto stage = prepare_warm_retouch_clone_stage(
            *retouch,
            dimensions,
            level_zero_to_raster_scale_x,
            level_zero_to_raster_scale_y,
            context
        );
        return stage.has_value() ? std::optional<WarmGpuNeighbourhoodStage>{std::move(*stage)}
                                 : std::nullopt;
    }
    if (auto stage = prepare_warm_technical_detail_stage(
            nodes,
            plan,
            dimensions,
            working_space,
            level_zero_to_raster_scale_x,
            level_zero_to_raster_scale_y
        );
        stage.has_value()) {
        return isolate_stage_post_program(std::move(stage));
    }
    if (auto stage = prepare_warm_texture_clarity_stage(
            nodes,
            plan,
            dimensions,
            level_zero_to_raster_scale_x,
            level_zero_to_raster_scale_y
        );
        stage.has_value()) {
        return isolate_stage_post_program(std::move(stage));
    }
    if (auto stage = prepare_warm_local_contrast_stage(
            nodes,
            plan,
            dimensions,
            level_zero_to_raster_scale_x,
            level_zero_to_raster_scale_y
        );
        stage.has_value()) {
        return isolate_stage_post_program(std::move(stage));
    }
    if (auto stage = prepare_warm_selective_tone_stage(
            nodes,
            plan,
            dimensions,
            working_space,
            level_zero_to_raster_scale_x,
            level_zero_to_raster_scale_y
        );
        stage.has_value()) {
        return isolate_stage_post_program(std::move(stage));
    }
    if (auto stage = prepare_warm_texture_stage(
            nodes,
            plan,
            dimensions,
            level_zero_to_raster_scale_x,
            level_zero_to_raster_scale_y
        );
        stage.has_value()) {
        return isolate_stage_post_program(std::move(stage));
    }
    if (auto stage = prepare_warm_clarity_stage(
            nodes,
            plan,
            dimensions,
            level_zero_to_raster_scale_x,
            level_zero_to_raster_scale_y
        );
        stage.has_value()) {
        return isolate_stage_post_program(std::move(stage));
    }
    return std::nullopt;
}

} // namespace shadow::image::detail
