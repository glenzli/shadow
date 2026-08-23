#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/decoder_types.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace shadow::image {

struct SensorClippingMask;
struct HighlightChromaRiskMap;

enum class AdjustmentLocality : std::uint8_t {
    pixel_local,
    neighborhood,
};

// Conservative integer support, in pixels of the raster being executed. Sequential
// neighborhood operations require the sum of their footprints, not merely the maximum.
struct AdjustmentFootprint final {
    std::uint32_t horizontal_radius = 0;
    std::uint32_t vertical_radius = 0;

    auto operator<=>(const AdjustmentFootprint&) const = default;
};

// Backend-neutral execution-plan identity. CPU, Metal, and future backends may consume the
// same compiled ordering contract; a semantic change to segmentation, neutral-node elision,
// or footprint accumulation must publish a new version and canonical identity.
inline constexpr std::uint32_t edit_execution_plan_identity_version = 1;
inline constexpr std::string_view edit_execution_plan_identity = "shadow.edit-execution-plan.v1";

// One executable node retained by a compiled plan. node_index always addresses the original
// source span, so a backend can recover immutable parameters without copying heavyweight LUTs.
struct EditExecutionStep final {
    std::size_t node_index = 0;
    AdjustmentOperation operation = AdjustmentOperation::exposure;

    auto operator<=>(const EditExecutionStep&) const = default;
};

// A maximal run of executable nodes with the same locality. Disabled and exactly neutral nodes
// are absent from steps and do not split a run. first_node_index/past_last_node_index bound the
// retained source nodes (and may therefore contain omitted nodes between two retained steps).
struct EditExecutionSegment final {
    AdjustmentLocality locality = AdjustmentLocality::pixel_local;
    std::size_t first_node_index = 0;
    std::size_t past_last_node_index = 0;
    std::vector<EditExecutionStep> steps;
    AdjustmentFootprint cumulative_footprint;

    auto operator<=>(const EditExecutionSegment&) const = default;
};

struct EditExecutionPlan final {
    std::size_t source_node_count = 0;
    std::vector<EditExecutionSegment> segments;
    // Sequential neighborhood support composes additively across segment boundaries.
    AdjustmentFootprint cumulative_footprint;

    auto operator<=>(const EditExecutionPlan&) const = default;
};

// Pass-aware locality for scheduling. The legacy operation-only overload is
// intentionally conservative for callers that do not retain parameters.
[[nodiscard]] AdjustmentLocality locality(const AdjustmentParameters& parameters) noexcept;
[[nodiscard]] AdjustmentLocality locality(AdjustmentOperation operation) noexcept;
// The supplied scales are level-0-to-raster sampling densities. Invalid/non-finite scales or
// malformed parameters fail closed; callers normally validate the full node plan first.
[[nodiscard]] AdjustmentFootprint footprint(
    const AdjustmentParameters& parameters,
    double level_zero_to_raster_scale_x = 1.0,
    double level_zero_to_raster_scale_y = 1.0,
    Dimensions raster_dimensions = {}
);

// Validates the complete adjustment plan without requiring image pixels. All nodes, including
// disabled ones, are checked for supported versions, finite parameters, and valid perceptual
// tone-curve geometry/slopes. This lets callers reject malformed work before an expensive
// decode. Pixel-dependent overflow remains the responsibility of execute_adjustment_nodes().
void validate_adjustment_nodes(std::span<const AdjustmentNode> nodes);

// Validates every source node first, including disabled nodes, then compiles enabled,
// non-neutral nodes into maximal locality segments without reordering operations. The supplied
// scales have the same level-0-to-raster meaning as footprint(). A plan contains indices rather
// than parameter copies and is therefore valid only while the source node span is unchanged.
[[nodiscard]] EditExecutionPlan compile_edit_execution_plan(
    std::span<const AdjustmentNode> nodes,
    double level_zero_to_raster_scale_x = 1.0,
    double level_zero_to_raster_scale_y = 1.0
);

// Global raster coordinates keep deterministic grain and radial effects identical between a
// full proxy and independently rendered detail tiles. Zero full dimensions mean "use input".
struct AdjustmentExecutionContext final {
    std::uint32_t origin_x = 0;
    std::uint32_t origin_y = 0;
    Dimensions full_dimensions{};
    // Immutable source evidence, borrowed only for this synchronous execution.  It is not a
    // Recipe parameter: RAW preparation owns the physical sensor-white observation and may
    // omit it for non-RAW sources.  Selective tone uses it only when a negative highlight/white
    // recovery would otherwise expose chroma that the sensor did not measure.
    const SensorClippingMask* sensor_clipping_mask = nullptr;
    const HighlightChromaRiskMap* highlight_chroma_risk_map = nullptr;
};

} // namespace shadow::image
