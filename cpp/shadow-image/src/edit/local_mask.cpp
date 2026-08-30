#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include "local_mask_coverage.hpp"
#include "local_mask_validation.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>

namespace shadow::image {

namespace {

void mix_masked_layer(
    FloatRgbImage& destination,
    const FloatRgbImage& source,
    const LocalMask& mask,
    const double opacity,
    const AdjustmentExecutionContext context,
    const Dimensions full,
    const detail::LocalMaskCoverageRaster* captured_coverage
) {
    const std::optional<detail::PreparedLocalMaskCoverage> prepared =
        captured_coverage == nullptr
        ? std::optional<detail::PreparedLocalMaskCoverage>{
              detail::prepare_local_mask_coverage(mask, source, full)}
        : std::nullopt;
    const std::size_t stride = destination.row_stride_bytes / sizeof(float);
    const auto width = static_cast<std::size_t>(destination.dimensions.width);
    const auto height = static_cast<std::size_t>(destination.dimensions.height);
    for (std::size_t row = 0U; row < height; ++row) {
        const double y = captured_coverage == nullptr
            ? (static_cast<double>(context.origin_y) + static_cast<double>(row) + 0.5)
                / static_cast<double>(full.height)
            : 0.0;
        for (std::size_t column = 0U; column < width; ++column) {
            const std::size_t sample = row * stride + column * 3U;
            const double coverage = captured_coverage != nullptr
                ? static_cast<double>(
                      captured_coverage->samples[row * width + column])
                : detail::local_mask_coverage_at(
                      *prepared,
                      (static_cast<double>(context.origin_x)
                           + static_cast<double>(column) + 0.5)
                          / static_cast<double>(full.width),
                      y,
                      detail::Vector3{
                          static_cast<double>(source.samples[sample]),
                          static_cast<double>(source.samples[sample + 1U]),
                          static_cast<double>(source.samples[sample + 2U]),
                      }
                  );
            const double alpha = opacity * coverage;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const double before = static_cast<double>(source.samples[sample + channel]);
                const double after = static_cast<double>(destination.samples[sample + channel]);
                const double mixed = std::fma(alpha, after - before, before);
                if (!std::isfinite(mixed)
                    || mixed < static_cast<double>(std::numeric_limits<float>::lowest())
                    || mixed > static_cast<double>(std::numeric_limits<float>::max())) {
                    throw EditError(
                        EditErrorCode::numeric_overflow,
                        std::nullopt,
                        "local-mask layer blend produced an invalid RGB sample"
                    );
                }
                destination.samples[sample + channel] = static_cast<float>(mixed);
            }
        }
    }
}

} // namespace

FloatRgbImage execute_adjustment_layers(
    const FloatRgbImage& input,
    const std::span<const AdjustmentLayer> layers,
    const AdjustmentExecutionContext context
) {
    auto executed = detail::execute_adjustment_layers_with_mask_coverage(
        input,
        layers,
        std::nullopt,
        context,
        {}
    );
    if (!executed.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable adjustment-layer execution was unexpectedly cancelled"
        );
    }
    return std::move(executed->pixels);
}

namespace detail {

std::optional<AdjustmentLayersWithMaskCoverage> execute_adjustment_layers_with_mask_coverage(
    const FloatRgbImage& input,
    const std::span<const AdjustmentLayer> layers,
    const std::optional<std::uint32_t> target_layer_index,
    const AdjustmentExecutionContext context,
    const std::stop_token cancellation,
    const std::optional<std::uint32_t> target_component_index
) {
    if (target_layer_index.has_value()
        && static_cast<std::size_t>(*target_layer_index) >= layers.size()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "mask coverage target layer index is outside the adjustment-layer plan"
        );
    }
    if (target_component_index.has_value() && !target_layer_index.has_value()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "mask coverage component requires a target layer"
        );
    }
    const Dimensions full = validate_adjustment_layer_plan(input, layers, context);
    FloatRgbImage output = input;
    std::optional<LocalMaskCoverageRaster> mask_coverage;
    for (std::size_t layer_index = 0U; layer_index < layers.size(); ++layer_index) {
        if (cancellation.stop_requested()) {
            return std::nullopt;
        }
        const AdjustmentLayer& layer = layers[layer_index];
        if (target_layer_index.has_value()
            && layer_index == static_cast<std::size_t>(*target_layer_index)
            && layer.mask.has_value()) {
            mask_coverage = render_local_mask_coverage(
                output,
                *layer.mask,
                context,
                full,
                cancellation,
                target_component_index
            );
            if (!mask_coverage.has_value()) {
                return std::nullopt;
            }
        }
        if (!layer.enabled || layer.opacity == 0.0) {
            continue;
        }
        if (!layer.mask.has_value() && layer.opacity == 1.0) {
            output = execute_adjustment_nodes(output, layer.nodes, context);
            continue;
        }
        const FloatRgbImage source = output;
        output = execute_adjustment_nodes(output, layer.nodes, context);
        if (layer.mask.has_value()) {
            const bool captured_target =
                target_layer_index.has_value()
                && layer_index == static_cast<std::size_t>(*target_layer_index)
                && mask_coverage.has_value() && !target_component_index.has_value();
            mix_masked_layer(
                output,
                source,
                *layer.mask,
                layer.opacity,
                context,
                full,
                captured_target ? &*mask_coverage : nullptr
            );
        } else {
            // Opacity without an explicit spatial mask is a whole-image blend.
            // A synthetic all-covered spatial mask would add needless per-pixel
            // coordinate work, so this path blends directly.
            const std::size_t count = output.samples.size();
            for (std::size_t index = 0U; index < count; ++index) {
                const double before = static_cast<double>(source.samples[index]);
                const double after = static_cast<double>(output.samples[index]);
                const double mixed = std::fma(layer.opacity, after - before, before);
                if (!std::isfinite(mixed)
                    || mixed < static_cast<double>(std::numeric_limits<float>::lowest())
                    || mixed > static_cast<double>(std::numeric_limits<float>::max())) {
                    throw EditError(
                        EditErrorCode::numeric_overflow,
                        std::nullopt,
                        "local-mask layer opacity blend produced an invalid RGB sample"
                    );
                }
                output.samples[index] = static_cast<float>(mixed);
            }
        }
    }
    if (cancellation.stop_requested()) {
        return std::nullopt;
    }
    return AdjustmentLayersWithMaskCoverage{
        .pixels = std::move(output),
        .mask_coverage = std::move(mask_coverage),
    };
}

} // namespace detail

} // namespace shadow::image
