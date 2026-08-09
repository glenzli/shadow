#include "warm_edit_gpu_geometry_plan.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

inline constexpr std::uint32_t warm_liquify_grid_axis_limit = 256U;
inline constexpr double warm_liquify_target_cell_side = 32.0;
inline constexpr std::size_t warm_liquify_reference_limit = 1'048'576U;
inline constexpr std::uint32_t warm_liquify_stamp_words = 8U;
inline constexpr std::uint32_t warm_liquify_cell_range_words = 2U;

[[nodiscard]] bool is_transposed(const PhotoQuarterTurn quarter_turn) noexcept {
    return quarter_turn == PhotoQuarterTurn::clockwise_90
           || quarter_turn == PhotoQuarterTurn::clockwise_270;
}

[[nodiscard]] bool
contains(const GeometryPixelRect& outer, const GeometryPixelRect& inner) noexcept {
    const std::uint64_t outer_right = static_cast<std::uint64_t>(outer.x) + outer.width;
    const std::uint64_t outer_bottom = static_cast<std::uint64_t>(outer.y) + outer.height;
    const std::uint64_t inner_right = static_cast<std::uint64_t>(inner.x) + inner.width;
    const std::uint64_t inner_bottom = static_cast<std::uint64_t>(inner.y) + inner.height;
    return inner.x >= outer.x && inner.y >= outer.y && inner_right <= outer_right
           && inner_bottom <= outer_bottom;
}

[[nodiscard]] GeometryPixelRect expand_rect(
    const GeometryPixelRect rect,
    const Dimensions dimensions,
    const double displacement_pixels
) {
    const double rounded = std::ceil(displacement_pixels);
    if (!std::isfinite(rounded)
        || rounded > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return {};
    }
    const std::uint32_t bound = static_cast<std::uint32_t>(rounded);
    const std::uint32_t left = std::min(rect.x, bound);
    const std::uint32_t top = std::min(rect.y, bound);
    const std::uint32_t right = std::min(
        dimensions.width - (rect.x + rect.width),
        bound
    );
    const std::uint32_t bottom = std::min(
        dimensions.height - (rect.y + rect.height),
        bound
    );
    return GeometryPixelRect{
        .x = rect.x - left,
        .y = rect.y - top,
        .width = rect.width + left + right,
        .height = rect.height + top + bottom,
    };
}

void append_word(
    std::vector<WarmPhotoLiquifyWord>& words,
    const std::uint32_t value
) {
    words.push_back(WarmPhotoLiquifyWord{.value = value});
}

void append_float(
    std::vector<WarmPhotoLiquifyWord>& words,
    const float value
) {
    append_word(words, std::bit_cast<std::uint32_t>(value));
}

struct WarmLiquifyLowering final {
    WarmPhotoLiquifyParameters parameters;
    std::vector<WarmPhotoLiquifyWord> words;
    double maximum_displacement_pixels = 0.0;
    std::string diagnostic;
};

struct WarmLiquifyStampAbi final {
    std::uint32_t kind = 0U;
    float center_x = 0.0F;
    float center_y = 0.0F;
    float displacement_x = 0.0F;
    float displacement_y = 0.0F;
    float reconstruction = 0.0F;
    float radius = 0.0F;
    float hardness = 0.0F;
};

[[nodiscard]] double float_replay_rounding_guard(
    const std::size_t operation_count,
    const double coordinate_extent,
    const double displacement_bound
) {
    // Each stamp performs bounded float arithmetic before one coordinate
    // subtraction. Four epsilons per replay step conservatively cover the
    // multiply/subtract chain; gamma_n accounts for accumulation.
    const double factor =
        static_cast<double>(operation_count) * 4.0
        * static_cast<double>(std::numeric_limits<float>::epsilon());
    if (factor >= 0.5) {
        return std::numeric_limits<double>::infinity();
    }
    return (coordinate_extent + displacement_bound) * factor / (1.0 - factor);
}

[[nodiscard]] WarmLiquifyLowering lower_liquify(
    const PreparedPhotoLiquify* const liquify
) {
    if (liquify == nullptr) {
        return {};
    }
    if (!liquify->valid()) {
        return WarmLiquifyLowering{
            .diagnostic = "photo Liquify received an invalid prepared stamp plan",
        };
    }

    const std::uint32_t width = liquify->source_dimensions.width;
    const std::uint32_t height = liquify->source_dimensions.height;
    std::vector<WarmLiquifyStampAbi> abi_stamps;
    abi_stamps.reserve(liquify->stamps.size());
    for (const PreparedPhotoLiquifyStamp& stamp : liquify->stamps) {
        const WarmLiquifyStampAbi abi{
            .kind = static_cast<std::uint32_t>(stamp.kind),
            .center_x = static_cast<float>(stamp.center_x),
            .center_y = static_cast<float>(stamp.center_y),
            .displacement_x = static_cast<float>(stamp.displacement_x),
            .displacement_y = static_cast<float>(stamp.displacement_y),
            .reconstruction = static_cast<float>(stamp.reconstruction),
            .radius = static_cast<float>(stamp.radius),
            .hardness = static_cast<float>(stamp.hardness),
        };
        if (abi.kind > 1U || !std::isfinite(abi.center_x) || !std::isfinite(abi.center_y)
            || !std::isfinite(abi.displacement_x)
            || !std::isfinite(abi.displacement_y) || !std::isfinite(abi.reconstruction)
            || abi.reconstruction < 0.0F || abi.reconstruction > 1.0F
            || !std::isfinite(abi.radius)
            || abi.radius <= 0.0F || !std::isfinite(abi.hardness)
            || abi.hardness < 0.0F || abi.hardness >= 1.0F) {
            return WarmLiquifyLowering{
                .diagnostic =
                    abi.hardness >= 1.0F
                    ? "hard-edged photo Liquify requires the CPU structural sampler"
                    : "photo Liquify cannot be represented by the Metal float ABI",
            };
        }
        abi_stamps.push_back(abi);
    }
    const auto grid_extent = [](const std::uint32_t extent) {
        const double cells =
            std::ceil(static_cast<double>(extent) / warm_liquify_target_cell_side);
        return std::clamp(
            static_cast<std::uint32_t>(cells),
            1U,
            warm_liquify_grid_axis_limit
        );
    };
    const std::uint32_t columns = grid_extent(width);
    const std::uint32_t rows = grid_extent(height);
    const float cell_width_f =
        static_cast<float>(static_cast<double>(width) / columns);
    const float cell_height_f =
        static_cast<float>(static_cast<double>(height) / rows);
    const double cell_width = static_cast<double>(cell_width_f);
    const double cell_height = static_cast<double>(cell_height_f);
    const std::size_t cell_count = static_cast<std::size_t>(columns) * rows;
    std::vector<std::vector<std::uint32_t>> cell_references(cell_count);

    std::size_t reference_count = 0U;
    double later_displacement_bound = 0.0;
    std::size_t later_operation_count = 0U;
    const double coordinate_extent = static_cast<double>(std::max(width, height));
    for (std::size_t reverse = abi_stamps.size(); reverse > 0U; --reverse) {
        const std::size_t stamp_index = reverse - 1U;
        const WarmLiquifyStampAbi& stamp = abi_stamps[stamp_index];
        // Before this stamp is evaluated, inverse replay has applied only
        // later stamps. Their displacement magnitudes form a conservative
        // bound around the initial Canvas coordinate. Any stamp whose support
        // could be reached from a cell is therefore present in that cell's
        // reverse-ordered candidate list; false positives are harmless.
        const double rounding_guard = float_replay_rounding_guard(
            later_operation_count,
            coordinate_extent,
            later_displacement_bound
        );
        if (!std::isfinite(rounding_guard)) {
            return WarmLiquifyLowering{
                .diagnostic =
                    "photo Liquify Metal replay exceeds the bounded float error budget",
            };
        }
        const double expanded_radius =
            static_cast<double>(stamp.radius) + later_displacement_bound
            + rounding_guard;
        const double lower_x = std::clamp(
            static_cast<double>(stamp.center_x) - expanded_radius,
            0.0,
            width - 1.0
        );
        const double upper_x = std::clamp(
            static_cast<double>(stamp.center_x) + expanded_radius,
            0.0,
            width - 1.0
        );
        const double lower_y = std::clamp(
            static_cast<double>(stamp.center_y) - expanded_radius,
            0.0,
            height - 1.0
        );
        const double upper_y = std::clamp(
            static_cast<double>(stamp.center_y) + expanded_radius,
            0.0,
            height - 1.0
        );
        const auto cell_index = [](const double coordinate,
                                   const double cell_side,
                                   const std::uint32_t extent) {
            return std::min(
                static_cast<std::uint32_t>(coordinate / cell_side),
                extent - 1U
            );
        };
        // Host planning uses double while the Metal ABI evaluates float
        // coordinates. Retain a one-cell guard around every analytical
        // support so a rounded coordinate on a cell boundary can only add a
        // false positive, never lose a reachable stamp.
        const std::uint32_t support_first_column =
            cell_index(lower_x, cell_width, columns);
        const std::uint32_t support_last_column =
            cell_index(upper_x, cell_width, columns);
        const std::uint32_t support_first_row =
            cell_index(lower_y, cell_height, rows);
        const std::uint32_t support_last_row =
            cell_index(upper_y, cell_height, rows);
        const std::uint32_t first_column =
            support_first_column == 0U ? 0U : support_first_column - 1U;
        const std::uint32_t last_column =
            std::min(support_last_column + 1U, columns - 1U);
        const std::uint32_t first_row =
            support_first_row == 0U ? 0U : support_first_row - 1U;
        const std::uint32_t last_row =
            std::min(support_last_row + 1U, rows - 1U);
        const std::size_t added =
            static_cast<std::size_t>(last_column - first_column + 1U)
            * static_cast<std::size_t>(last_row - first_row + 1U);
        if (added > warm_liquify_reference_limit - reference_count) {
            return WarmLiquifyLowering{
                .diagnostic =
                    "photo Liquify conservative Metal index exceeds the fixed reference bound",
            };
        }
        for (std::uint32_t row = first_row; row <= last_row; ++row) {
            for (std::uint32_t column = first_column; column <= last_column; ++column) {
                cell_references[static_cast<std::size_t>(row) * columns + column]
                    .push_back(static_cast<std::uint32_t>(stamp_index));
            }
        }
        reference_count += added;
        const double displacement =
            stamp.kind == 0U
            ? std::hypot(
                  static_cast<double>(stamp.displacement_x),
                  static_cast<double>(stamp.displacement_y)
              )
            : 0.0;
        later_displacement_bound = std::nextafter(
            later_displacement_bound + displacement,
            std::numeric_limits<double>::infinity()
        );
        ++later_operation_count;
    }

    WarmLiquifyLowering result;
    result.words.reserve(
        abi_stamps.size() * warm_liquify_stamp_words
        + cell_count * warm_liquify_cell_range_words + reference_count
    );
    for (const WarmLiquifyStampAbi& stamp : abi_stamps) {
        append_word(result.words, stamp.kind);
        append_float(result.words, stamp.center_x);
        append_float(result.words, stamp.center_y);
        append_float(result.words, stamp.displacement_x);
        append_float(result.words, stamp.displacement_y);
        append_float(result.words, stamp.reconstruction);
        append_float(result.words, stamp.radius);
        append_float(result.words, stamp.hardness);
    }
    const std::uint32_t cell_range_offset_words =
        static_cast<std::uint32_t>(result.words.size());
    std::uint32_t reference_offset = 0U;
    for (const auto& references : cell_references) {
        append_word(result.words, reference_offset);
        append_word(result.words, static_cast<std::uint32_t>(references.size()));
        reference_offset += static_cast<std::uint32_t>(references.size());
    }
    const std::uint32_t reference_offset_words =
        static_cast<std::uint32_t>(result.words.size());
    for (const auto& references : cell_references) {
        for (const std::uint32_t stamp_index : references) {
            append_word(result.words, stamp_index);
        }
    }
    result.parameters = WarmPhotoLiquifyParameters{
        .source_width = width,
        .source_height = height,
        .grid_columns = columns,
        .grid_rows = rows,
        .cell_width = cell_width_f,
        .cell_height = cell_height_f,
        .stamp_count = static_cast<std::uint32_t>(abi_stamps.size()),
        .cell_range_offset_words = cell_range_offset_words,
        .reference_offset_words = reference_offset_words,
        .reference_count = static_cast<std::uint32_t>(reference_count),
    };
    const double final_rounding_guard = float_replay_rounding_guard(
        abi_stamps.size(),
        coordinate_extent,
        later_displacement_bound
    );
    if (!std::isfinite(final_rounding_guard)) {
        return WarmLiquifyLowering{
            .diagnostic =
                "photo Liquify Metal replay exceeds the bounded float error budget",
        };
    }
    result.maximum_displacement_pixels =
        later_displacement_bound + final_rounding_guard;
    return result;
}

} // namespace

bool WarmGpuGeometryPlan::valid() const noexcept {
    const auto& value = parameters;
    const std::uint64_t input_pixels =
        static_cast<std::uint64_t>(value.input_width) * value.input_height;
    const std::uint64_t output_pixels =
        static_cast<std::uint64_t>(value.output_width) * value.output_height;
    const bool geometry_valid = value.input_width > 0U && value.input_height > 0U
           && value.input_row_floats >= value.input_width * 3U && value.source_crop_width > 0U
           && value.source_crop_height > 0U && value.output_canvas_width > 0U
           && value.output_canvas_height > 0U && value.output_width > 0U && value.output_height > 0U
           && value.output_origin_x <= value.output_canvas_width
           && value.output_origin_y <= value.output_canvas_height
           && value.output_width <= value.output_canvas_width - value.output_origin_x
           && value.output_height <= value.output_canvas_height - value.output_origin_y
           && value.quarter_turn <= 3U && value.flip_horizontal <= 1U && value.flip_vertical <= 1U
           && std::isfinite(value.straighten_cosine) && std::isfinite(value.straighten_sine)
           && std::isfinite(value.perspective_vertical)
           && value.perspective_vertical >= -1.0F && value.perspective_vertical <= 1.0F
           && std::isfinite(value.perspective_horizontal)
           && value.perspective_horizontal >= -1.0F && value.perspective_horizontal <= 1.0F
           && output_pixels <= input_pixels
           && output_dimensions == Dimensions{value.output_width, value.output_height}
           && std::isfinite(output_level_zero_to_raster_scale_x)
           && output_level_zero_to_raster_scale_x > 0.0
           && std::isfinite(output_level_zero_to_raster_scale_y)
           && output_level_zero_to_raster_scale_y > 0.0;
    if (!geometry_valid) {
        return false;
    }
    const auto& liquify = liquify_parameters;
    if (liquify.stamp_count == 0U) {
        return liquify.source_width == 0U && liquify.source_height == 0U
               && liquify.grid_columns == 0U && liquify.grid_rows == 0U
               && liquify.reference_count == 0U && liquify_words.empty();
    }
    const std::uint64_t cell_count =
        static_cast<std::uint64_t>(liquify.grid_columns) * liquify.grid_rows;
    const std::uint64_t stamp_words =
        static_cast<std::uint64_t>(liquify.stamp_count) * warm_liquify_stamp_words;
    const std::uint64_t reference_words =
        static_cast<std::uint64_t>(liquify.reference_offset_words)
        + liquify.reference_count;
    if (
        liquify.source_width == 0U || liquify.source_height == 0U
        || liquify.grid_columns == 0U
        || liquify.grid_columns > warm_liquify_grid_axis_limit || liquify.grid_rows == 0U
        || liquify.grid_rows > warm_liquify_grid_axis_limit
        || !std::isfinite(liquify.cell_width) || liquify.cell_width <= 0.0F
        || !std::isfinite(liquify.cell_height) || liquify.cell_height <= 0.0F
        || liquify.stamp_count > maximum_prepared_photo_liquify_stamps
        || liquify.reference_count > warm_liquify_reference_limit
        || liquify.cell_range_offset_words != stamp_words
        || liquify.reference_offset_words
            != stamp_words + cell_count * warm_liquify_cell_range_words
        || reference_words != liquify_words.size()
    ) {
        return false;
    }
    for (std::uint64_t cell = 0U; cell < cell_count; ++cell) {
        const std::size_t offset_index = static_cast<std::size_t>(
            liquify.cell_range_offset_words + cell * warm_liquify_cell_range_words
        );
        const std::uint32_t offset = liquify_words[offset_index].value;
        const std::uint32_t count = liquify_words[offset_index + 1U].value;
        if (offset > liquify.reference_count
            || count > liquify.reference_count - offset) {
            return false;
        }
        for (std::uint32_t reference = 0U; reference < count; ++reference) {
            const std::size_t reference_index = static_cast<std::size_t>(
                liquify.reference_offset_words + offset + reference
            );
            if (liquify_words[reference_index].value >= liquify.stamp_count) {
                return false;
            }
        }
    }
    return true;
}

WarmGpuGeometryPreparation prepare_warm_gpu_geometry_plan(
    const Dimensions resident_dimensions,
    const std::uint32_t input_row_floats,
    const double input_level_zero_to_raster_scale_x,
    const double input_level_zero_to_raster_scale_y,
    const WarmEditGpuGeometryContext& context
) {
    const auto failed = [](std::string diagnostic) {
        return WarmGpuGeometryPreparation{
            .plan = std::nullopt,
            .diagnostic = std::move(diagnostic),
        };
    };
    if (resident_dimensions.width == 0U || resident_dimensions.height == 0U
        || resident_dimensions.width > std::numeric_limits<std::uint32_t>::max() / 3U
        || input_row_floats < resident_dimensions.width * 3U
        || !std::isfinite(input_level_zero_to_raster_scale_x)
        || input_level_zero_to_raster_scale_x <= 0.0
        || !std::isfinite(input_level_zero_to_raster_scale_y)
        || input_level_zero_to_raster_scale_y <= 0.0) {
        return failed("photo geometry received an invalid resident source layout");
    }
    if (context.source_tile_rect.width != resident_dimensions.width
        || context.source_tile_rect.height != resident_dimensions.height
        || context.layout.source_crop.width == 0U || context.layout.source_crop.height == 0U
        || context.layout.output_dimensions.width == 0U
        || context.layout.output_dimensions.height == 0U) {
        return failed("photo geometry does not describe the resident source tile");
    }

    WarmLiquifyLowering liquify = lower_liquify(context.liquify);
    if (!liquify.diagnostic.empty()) {
        return failed(std::move(liquify.diagnostic));
    }
    GeometryPixelRect required;
    try {
        required = photo_geometry_source_rect_for_output(
            context.layout,
            context.geometry,
            context.output_rect
        );
    } catch (const std::exception& error) {
        return failed(error.what());
    }
    if (context.liquify != nullptr) {
        required = expand_rect(
            required,
            context.liquify->source_dimensions,
            std::max(
                context.liquify->maximum_displacement_pixels,
                liquify.maximum_displacement_pixels
            )
        );
        if (required.width == 0U || required.height == 0U) {
            return failed("photo Liquify displacement bound exceeds the Metal address space");
        }
    }
    if (!contains(context.source_tile_rect, required)) {
        return failed("photo geometry source tile does not contain the sampling footprint");
    }

    const double radians =
        context.geometry.straighten_degrees * 3.141592653589793238462643383279502884 / 180.0;
    WarmGpuGeometryPlan plan{
        .parameters =
            WarmPhotoGeometryParameters{
                .input_width = resident_dimensions.width,
                .input_height = resident_dimensions.height,
                .input_row_floats = input_row_floats,
                .source_tile_origin_x = context.source_tile_rect.x,
                .source_tile_origin_y = context.source_tile_rect.y,
                .source_crop_origin_x = context.layout.source_crop.x,
                .source_crop_origin_y = context.layout.source_crop.y,
                .source_crop_width = context.layout.source_crop.width,
                .source_crop_height = context.layout.source_crop.height,
                .output_canvas_width = context.layout.output_dimensions.width,
                .output_canvas_height = context.layout.output_dimensions.height,
                .output_origin_x = context.output_rect.x,
                .output_origin_y = context.output_rect.y,
                .output_width = context.output_rect.width,
                .output_height = context.output_rect.height,
                .quarter_turn = static_cast<std::uint32_t>(context.geometry.quarter_turn),
                .flip_horizontal = context.geometry.flip_horizontal ? 1U : 0U,
                .flip_vertical = context.geometry.flip_vertical ? 1U : 0U,
                .straighten_cosine = static_cast<float>(std::cos(radians)),
                .straighten_sine = static_cast<float>(std::sin(radians)),
                .perspective_vertical =
                    static_cast<float>(context.geometry.perspective_vertical),
                .perspective_horizontal =
                    static_cast<float>(context.geometry.perspective_horizontal),
            },
        .liquify_parameters = liquify.parameters,
        .liquify_words = std::move(liquify.words),
        .output_dimensions =
            {
                context.output_rect.width,
                context.output_rect.height,
            },
        .output_level_zero_to_raster_scale_x = is_transposed(context.geometry.quarter_turn)
                                                   ? input_level_zero_to_raster_scale_y
                                                   : input_level_zero_to_raster_scale_x,
        .output_level_zero_to_raster_scale_y = is_transposed(context.geometry.quarter_turn)
                                                   ? input_level_zero_to_raster_scale_x
                                                   : input_level_zero_to_raster_scale_y,
    };
    if (!plan.valid()) {
        return failed("photo geometry exceeds the resident Metal output contract");
    }
    return WarmGpuGeometryPreparation{
        .plan = std::move(plan),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
