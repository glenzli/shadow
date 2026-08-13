#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/lut.hpp>
#include <shadow/image/retouch.hpp>
#include <shadow/image/working_rgb.hpp>

#include "adjustment_node_diagnostics.hpp"
#include "creative_detail_grading.hpp"
#include "edit_execution_validation.hpp"
#include "finishing_effects_cpu.hpp"
#include "guided_selective_tone.hpp"
#include "oklab_color_warper.hpp"
#include "perceptual_color.hpp"
#include "perceptual_contrast.hpp"
#include "retouch_source_transform.hpp"
#include "rgb_pixel_traversal.hpp"
#include "scalar_neighborhood_filters.hpp"
#include "technical_detail_cpu.hpp"
#include "tone_curve_internal.hpp"
#include "working_color_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace shadow::image {

namespace {

[[nodiscard]] bool normalized_amount(const double value) noexcept {
    return std::isfinite(value) && value >= -1.0 && value <= 1.0;
}

using detail::apply_color_matrix;
using detail::apply_creative_detail_grading_cpu;
using detail::apply_finishing_effects_cpu;
using detail::apply_oklab_color_warper_cpu;
using detail::apply_perceptual_color_cpu;
using detail::apply_prepared_guided_selective_tone_cpu;
using detail::apply_prepared_oklab_lightness_tone_curve;
using detail::apply_prepared_oklab_opponent_tone_curves;
using detail::apply_prepared_perceptual_contrast_cpu;
using detail::apply_technical_detail_cpu;
using detail::classify_perceptual_color;
using detail::creative_detail_footprint;
using detail::guided_selective_tone_is_neutral;
using detail::Matrix3;
using detail::oklab_color_warper_is_neutral;
using detail::oklab_to_working_rgb;
using detail::PerceptualColorStages;
using detail::prepare_guided_selective_tone;
using detail::prepare_oklab_lightness_tone_curve_node;
using detail::prepare_oklab_opponent_tone_curves_node;
using detail::prepare_perceptual_contrast;
using detail::prepare_rgb_white_balance_matrix;
using detail::prepare_working_space_transform;
using detail::PreparedOklabOpponentToneCurves;
using detail::PreparedPerceptualContrast;
using detail::PreparedSmoothToneCurve;
using detail::PreparedToneCurveAdjustment;
using detail::technical_detail_footprint;
using detail::throw_node_error;
using detail::transform_rgb_pixels;
using detail::validate_edit_execution_context;
using detail::validate_edit_image;
using detail::validate_guided_selective_tone;
using detail::validate_oklab_color_warper;
using detail::validate_perceptual_color;
using detail::Vector3;
using detail::working_rgb_to_oklab;
using detail::WorkingSpaceTransform;

struct PreparedAdjustmentNode final {
    PreparedToneCurveAdjustment tone_curve;
    std::optional<PreparedPerceptualContrast> perceptual_contrast;
};

[[nodiscard]] PreparedAdjustmentNode
validate_node(const AdjustmentNode& node, const std::size_t index) {
    const bool oklab_lightness_tone_curve =
        std::holds_alternative<OklabLightnessToneCurve>(node.parameters);
    const bool oklab_opponent_tone_curves =
        std::holds_alternative<OklabOpponentToneCurves>(node.parameters);
    const bool selective_tone = std::holds_alternative<SelectiveToneAdjustment>(node.parameters);
    const bool perceptual_color =
        std::holds_alternative<PerceptualColorAdjustment>(node.parameters);
    const bool oklab_color_warper =
        std::holds_alternative<OklabColorWarperAdjustment>(node.parameters);
    const bool detail_effects = std::holds_alternative<SharpenAdjustment>(node.parameters);
    const std::uint32_t expected_parameter_schema =
        oklab_lightness_tone_curve   ? oklab_lightness_tone_curve_parameter_schema_version
        : oklab_opponent_tone_curves ? oklab_opponent_tone_curve_parameter_schema_version
        : selective_tone             ? selective_tone_parameter_schema_version
        : perceptual_color           ? perceptual_color_parameter_schema_version
        : oklab_color_warper         ? oklab_color_warper_parameter_schema_version
        : detail_effects             ? detail_effects_parameter_schema_version
                                     : adjustment_parameter_schema_version;
    const std::uint32_t expected_implementation =
        oklab_lightness_tone_curve   ? oklab_lightness_tone_curve_implementation_version
        : oklab_opponent_tone_curves ? oklab_opponent_tone_curve_implementation_version
        : selective_tone             ? selective_tone_implementation_version
        : perceptual_color           ? perceptual_color_implementation_version
        : oklab_color_warper         ? oklab_color_warper_implementation_version
                                     : adjustment_implementation_version;
    const bool supported_detail_pass =
        detail_effects
        && ((std::get<SharpenAdjustment>(node.parameters).execution_pass
                 == DetailEffectsExecutionPass::technical_detail
             && node.implementation_version == technical_detail_implementation_version)
            || (std::get<SharpenAdjustment>(node.parameters).execution_pass
                    == DetailEffectsExecutionPass::color_grading
                && node.implementation_version == color_grading_implementation_version)
            || (std::get<SharpenAdjustment>(node.parameters).execution_pass
                    == DetailEffectsExecutionPass::finishing_effects
                && node.implementation_version == finishing_effects_implementation_version));
    if (node.parameter_schema_version != expected_parameter_schema
        || (!detail_effects && node.implementation_version != expected_implementation)
        || (detail_effects && !supported_detail_pass)) {
        throw_node_error(
            EditErrorCode::unsupported_version,
            index,
            node,
            oklab_lightness_tone_curve
                ? "Oklab lightness curve requires parameter schema 1 and implementation 1"
            : oklab_opponent_tone_curves
                ? "Oklab opponent curves require parameter schema 1 and implementation 1"
            : selective_tone   ? "selective tone requires the current guided-mask contract"
            : perceptual_color ? "perceptual color requires the current complete contract"
            : oklab_color_warper
                ? "Oklab Color Warper requires parameter schema 1 and implementation 1"
            : detail_effects ? "Detail & Effects requires the current split-pass contract"
                             : "only parameter schema 1 and implementation 1 are supported"
        );
    }

    PreparedAdjustmentNode prepared;
    std::visit(
        [&node, index, &prepared](const auto& parameters) {
            using Parameters = std::decay_t<decltype(parameters)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
                if (parameters.stops == 0.0) {
                    return;
                }
                const double gain = std::exp2(parameters.stops);
                if (!std::isfinite(parameters.stops) || !std::isfinite(gain) || gain <= 0.0) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "exposure stops must produce a finite, positive gain"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, ContrastAdjustment>) {
                prepared.perceptual_contrast = prepare_perceptual_contrast(parameters, node, index);
            } else if constexpr (std::is_same_v<Parameters, OklabLightnessToneCurve>) {
                prepared.tone_curve =
                    prepare_oklab_lightness_tone_curve_node(parameters, node, index);
            } else if constexpr (std::is_same_v<Parameters, OklabOpponentToneCurves>) {
                prepared.tone_curve =
                    prepare_oklab_opponent_tone_curves_node(parameters, node, index);
            } else if constexpr (std::is_same_v<Parameters, RgbWhiteBalanceAdjustment>) {
                if (!normalized_amount(parameters.temperature)
                    || !normalized_amount(parameters.tint)) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "RGB white balance temperature and tint must be within [-1, 1]"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, SaturationAdjustment>) {
                if (!std::isfinite(parameters.factor) || parameters.factor < 0.0) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "saturation factor must be finite and non-negative"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                validate_guided_selective_tone(parameters, node, index);
            } else if constexpr (std::is_same_v<Parameters, PerceptualColorAdjustment>) {
                validate_perceptual_color(parameters, node, index);
            } else if constexpr (std::is_same_v<Parameters, OklabColorWarperAdjustment>) {
                validate_oklab_color_warper(parameters, node, index);
            } else if constexpr (std::is_same_v<Parameters, CubeLutAdjustment>) {
                const std::size_t size = parameters.lut.size;
                const bool empty_neutral =
                    parameters.intensity == 0.0 && size == 0U && parameters.lut.entries.empty();
                const bool valid_shape = size >= 2U && size <= 65U
                                         && parameters.lut.entries.size() == size * size * size;
                const bool valid_domain =
                    std::ranges::all_of(
                        parameters.lut.domain_min,
                        [](const float value) { return std::isfinite(value); }
                    )
                    && std::ranges::all_of(
                        parameters.lut.domain_max,
                        [](const float value) { return std::isfinite(value); }
                    )
                    && parameters.lut.domain_min[0] < parameters.lut.domain_max[0]
                    && parameters.lut.domain_min[1] < parameters.lut.domain_max[1]
                    && parameters.lut.domain_min[2] < parameters.lut.domain_max[2];
                const bool finite_entries =
                    std::ranges::all_of(parameters.lut.entries, [](const auto& entry) {
                        return std::ranges::all_of(entry, [](const float value) {
                            return std::isfinite(value);
                        });
                    });
                if (!std::isfinite(parameters.intensity) || parameters.intensity < 0.0
                    || parameters.intensity > 1.0
                    || (!empty_neutral && (!valid_shape || !valid_domain || !finite_entries))) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "3D LUT requires a valid cube and intensity within [0, 1]"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, SpotHealAdjustment>) {
                try {
                    validate_spot_heal(parameters);
                } catch (const EditError& error) {
                    throw_node_error(error.code(), index, node, error.what());
                }
            } else if constexpr (std::is_same_v<Parameters, SharpenAdjustment>) {
                const auto unit = [](const double value) {
                    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
                };
                const auto signed_unit = [](const double value) {
                    return std::isfinite(value) && value >= -1.0 && value <= 1.0;
                };
                if (!std::isfinite(parameters.amount) || parameters.amount < 0.0
                    || parameters.amount > 2.0 || !std::isfinite(parameters.radius)
                    || parameters.radius < 0.1 || parameters.radius > 5.0
                    || !std::isfinite(parameters.threshold) || parameters.threshold < 0.0
                    || parameters.threshold > 1.0 || !std::isfinite(parameters.masking)
                    || parameters.masking < 0.0 || parameters.masking > 1.0
                    || !signed_unit(parameters.clarity) || !signed_unit(parameters.texture)
                    || !signed_unit(parameters.local_contrast)
                    || !unit(parameters.local_contrast_scale) || !unit(parameters.denoise_luminance)
                    || !unit(parameters.denoise_detail) || !unit(parameters.denoise_color)
                    || !signed_unit(parameters.dehaze) || !unit(parameters.defringe_purple_amount)
                    || !unit(parameters.defringe_green_amount)
                    || !std::isfinite(parameters.defringe_purple_hue_low)
                    || parameters.defringe_purple_hue_low < 0.0
                    || parameters.defringe_purple_hue_low > 360.0
                    || !std::isfinite(parameters.defringe_purple_hue_high)
                    || parameters.defringe_purple_hue_high < 0.0
                    || parameters.defringe_purple_hue_high > 360.0
                    || parameters.defringe_purple_hue_low + 10.0
                           > parameters.defringe_purple_hue_high
                    || !std::isfinite(parameters.defringe_green_hue_low)
                    || parameters.defringe_green_hue_low < 0.0
                    || parameters.defringe_green_hue_low > 360.0
                    || !std::isfinite(parameters.defringe_green_hue_high)
                    || parameters.defringe_green_hue_high < 0.0
                    || parameters.defringe_green_hue_high > 360.0
                    || parameters.defringe_green_hue_low + 10.0 > parameters.defringe_green_hue_high
                    || !unit(parameters.shadows_saturation)
                    || !signed_unit(parameters.shadows_luminance)
                    || !unit(parameters.midtones_saturation)
                    || !signed_unit(parameters.midtones_luminance)
                    || !unit(parameters.highlights_saturation)
                    || !signed_unit(parameters.highlights_luminance)
                    || !unit(parameters.grading_blending)
                    || !signed_unit(parameters.grading_balance) || !unit(parameters.grain_amount)
                    || !unit(parameters.grain_size) || !unit(parameters.grain_roughness)
                    || !signed_unit(parameters.vignette_amount)
                    || !unit(parameters.vignette_midpoint)
                    || !signed_unit(parameters.vignette_roundness)
                    || !unit(parameters.vignette_feather) || !unit(parameters.vignette_highlights)
                    || !std::isfinite(parameters.shadows_hue) || parameters.shadows_hue < 0.0
                    || parameters.shadows_hue > 360.0 || !std::isfinite(parameters.midtones_hue)
                    || parameters.midtones_hue < 0.0 || parameters.midtones_hue > 360.0
                    || !std::isfinite(parameters.highlights_hue) || parameters.highlights_hue < 0.0
                    || parameters.highlights_hue > 360.0) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "Detail & Effects parameters are outside their declared bounds"
                    );
                }
            }
        },
        node.parameters
    );
    return prepared;
}

[[nodiscard]] std::vector<PreparedAdjustmentNode>
prepare_adjustment_nodes(const std::span<const AdjustmentNode> nodes) {
    std::vector<PreparedAdjustmentNode> prepared_nodes;
    prepared_nodes.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        prepared_nodes.push_back(validate_node(nodes[index], index));
    }
    return prepared_nodes;
}

[[nodiscard]] bool adjustment_is_neutral(
    const AdjustmentParameters& parameters,
    const PreparedAdjustmentNode& prepared
) {
    return std::visit(
        [&prepared](const auto& value) {
            using Parameters = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
                return value.stops == 0.0;
            } else if constexpr (std::is_same_v<Parameters, ContrastAdjustment>) {
                return prepared.perceptual_contrast->neutral();
            } else if constexpr (std::is_same_v<Parameters, OklabLightnessToneCurve>) {
                return std::get<PreparedSmoothToneCurve>(prepared.tone_curve).is_identity();
            } else if constexpr (std::is_same_v<Parameters, OklabOpponentToneCurves>) {
                return std::get<PreparedOklabOpponentToneCurves>(prepared.tone_curve).is_identity();
            } else if constexpr (std::is_same_v<Parameters, RgbWhiteBalanceAdjustment>) {
                return value.temperature == 0.0 && value.tint == 0.0;
            } else if constexpr (std::is_same_v<Parameters, SaturationAdjustment>) {
                return value.factor == 1.0;
            } else if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                return guided_selective_tone_is_neutral(value);
            } else if constexpr (std::is_same_v<Parameters, PerceptualColorAdjustment>) {
                return classify_perceptual_color(value).neutral();
            } else if constexpr (std::is_same_v<Parameters, OklabColorWarperAdjustment>) {
                return oklab_color_warper_is_neutral(value);
            } else if constexpr (std::is_same_v<Parameters, CubeLutAdjustment>) {
                return value.intensity == 0.0;
            } else if constexpr (std::is_same_v<Parameters, SpotHealAdjustment>) {
                return value.spots.empty() && value.strokes.empty();
            } else {
                static_assert(std::is_same_v<Parameters, SharpenAdjustment>);
                switch (value.execution_pass) {
                case DetailEffectsExecutionPass::technical_detail:
                    return value.amount == 0.0 && value.denoise_luminance == 0.0
                           && value.denoise_color == 0.0 && value.dehaze == 0.0
                           && value.defringe_purple_amount == 0.0
                           && value.defringe_green_amount == 0.0;
                case DetailEffectsExecutionPass::color_grading:
                    return value.clarity == 0.0 && value.texture == 0.0
                           && value.local_contrast == 0.0 && value.shadows_saturation == 0.0
                           && value.shadows_luminance == 0.0 && value.midtones_saturation == 0.0
                           && value.midtones_luminance == 0.0 && value.highlights_saturation == 0.0
                           && value.highlights_luminance == 0.0;
                case DetailEffectsExecutionPass::finishing_effects:
                    return value.grain_amount == 0.0 && value.vignette_amount == 0.0;
                }
                return false;
            }
        },
        parameters
    );
}

void apply_node(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t index,
    const PreparedAdjustmentNode& prepared,
    const AdjustmentExecutionContext& context
) {
    // The planner and executor intentionally share this exact classifier. Besides avoiding
    // redundant traversals in the CPU path, this prevents a GPU backend from eliding a node
    // that the reference executor would treat as observable.
    if (adjustment_is_neutral(node.parameters, prepared)) {
        return;
    }
    std::visit(
        [&image, &node, index, &prepared, &context](const auto& parameters) {
            using Parameters = std::decay_t<decltype(parameters)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
                const double gain = std::exp2(parameters.stops);
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [gain](const std::array<double, 3>& input) {
                        return std::array{
                            input[0] * gain,
                            input[1] * gain,
                            input[2] * gain,
                        };
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, ContrastAdjustment>) {
                apply_prepared_perceptual_contrast_cpu(
                    image,
                    node,
                    index,
                    *prepared.perceptual_contrast
                );
            } else if constexpr (std::is_same_v<Parameters, OklabLightnessToneCurve>) {
                const WorkingSpaceTransform color_transform =
                    prepare_working_space_transform(image.working_space, node, index);
                apply_prepared_oklab_lightness_tone_curve(
                    image,
                    std::get<PreparedSmoothToneCurve>(prepared.tone_curve),
                    color_transform,
                    node,
                    index
                );
            } else if constexpr (std::is_same_v<Parameters, OklabOpponentToneCurves>) {
                const WorkingSpaceTransform color_transform =
                    prepare_working_space_transform(image.working_space, node, index);
                apply_prepared_oklab_opponent_tone_curves(
                    image,
                    std::get<PreparedOklabOpponentToneCurves>(prepared.tone_curve),
                    color_transform,
                    node,
                    index
                );
            } else if constexpr (std::is_same_v<Parameters, OklabColorWarperAdjustment>) {
                apply_oklab_color_warper_cpu(image, node, index, parameters);
            } else if constexpr (std::is_same_v<Parameters, RgbWhiteBalanceAdjustment>) {
                if (parameters.temperature == 0.0 && parameters.tint == 0.0) {
                    return;
                }
                const Matrix3 adaptation =
                    prepare_rgb_white_balance_matrix(image.working_space, parameters, node, index);
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&adaptation](const std::array<double, 3>& input) {
                        return apply_color_matrix(adaptation, input);
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, SaturationAdjustment>) {
                // Saturation is a chroma operation, not a linear-RGB interpolation around a
                // working-space luma value.  The latter can make equal numeric changes look
                // very different across hues and gives a poor neutral axis for wide-gamut
                // working spaces.  Oklab lets this control scale perceptual chroma while
                // retaining lightness, and the working-space transforms keep the public node
                // independent of the particular D65 RGB primaries selected for the recipe.
                if (parameters.factor == 1.0) {
                    return;
                }
                const WorkingSpaceTransform color_transform =
                    prepare_working_space_transform(image.working_space, node, index);
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters, &color_transform](const Vector3& input) {
                        // Preserve the D65 neutral axis exactly.  Besides avoiding a needless
                        // matrix round trip, this makes neutral grays invariant for every
                        // saturation value, including negative scene-linear values.
                        if (input[0] == input[1] && input[1] == input[2]) {
                            return input;
                        }
                        Vector3 lab = working_rgb_to_oklab(color_transform, input);
                        lab[1] *= parameters.factor;
                        lab[2] *= parameters.factor;
                        // Do not clamp here. Scene-linear RGB can legitimately carry negative
                        // and super-white values, and gamut mapping belongs to the output
                        // transform. checked_edit_pixel_float() below still fails closed on
                        // non-finite or unrepresentable results.
                        return oklab_to_working_rgb(color_transform, lab);
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                const auto prepared_selective_tone = prepare_guided_selective_tone(
                    parameters,
                    image.level_zero_to_raster_scale_x,
                    image.level_zero_to_raster_scale_y
                );
                apply_prepared_guided_selective_tone_cpu(
                    image,
                    node,
                    index,
                    prepared_selective_tone
                );
            } else if constexpr (std::is_same_v<Parameters, PerceptualColorAdjustment>) {
                const PerceptualColorStages stages = classify_perceptual_color(parameters);
                apply_perceptual_color_cpu(image, node, index, parameters, stages);
            } else if constexpr (std::is_same_v<Parameters, CubeLutAdjustment>) {
                if (parameters.intensity == 0.0) {
                    return;
                }
                transform_rgb_pixels(image, index, node, [&parameters](const Vector3& input) {
                    const auto sampled = sample_cube_lut(
                        parameters.lut,
                        {
                            static_cast<float>(input[0]),
                            static_cast<float>(input[1]),
                            static_cast<float>(input[2]),
                        }
                    );
                    const double mix = parameters.intensity;
                    return Vector3{
                        input[0] + (static_cast<double>(sampled[0]) - input[0]) * mix,
                        input[1] + (static_cast<double>(sampled[1]) - input[1]) * mix,
                        input[2] + (static_cast<double>(sampled[2]) - input[2]) * mix,
                    };
                });
            } else if constexpr (std::is_same_v<Parameters, SpotHealAdjustment>) {
                apply_spot_heal(image, parameters, context);
            } else if constexpr (std::is_same_v<Parameters, SharpenAdjustment>) {
                switch (parameters.execution_pass) {
                case DetailEffectsExecutionPass::technical_detail:
                    // Technical recovery is deliberately scene-linear and
                    // pre-creative: it must not denoise or sharpen a LUT.
                    apply_technical_detail_cpu(image, node, index, parameters);
                    break;
                case DetailEffectsExecutionPass::color_grading:
                    // Perceptual clarity/texture and color wheels are creative
                    // transforms. The former only touches Oklab L; the latter
                    // works in hue/chroma, so they cooperate without an RGB
                    // channel-order dependency and both remain before a LUT.
                    apply_creative_detail_grading_cpu(image, node, index, parameters);
                    break;
                case DetailEffectsExecutionPass::finishing_effects:
                    // Grain and vignette are intentionally the last internal
                    // pass so LUT/grading do not alter their look.
                    apply_finishing_effects_cpu(image, node, index, parameters, context);
                    break;
                }
            }
        },
        node.parameters
    );
}

} // namespace

AdjustmentLocality locality(const AdjustmentOperation operation) noexcept {
    switch (operation) {
    case AdjustmentOperation::exposure:
    case AdjustmentOperation::contrast:
    case AdjustmentOperation::oklab_lightness_tone_curve:
    case AdjustmentOperation::oklab_opponent_tone_curves:
    case AdjustmentOperation::rgb_white_balance:
    case AdjustmentOperation::saturation:
    case AdjustmentOperation::perceptual_color:
    case AdjustmentOperation::oklab_color_warper:
    case AdjustmentOperation::lut_3d:
        return AdjustmentLocality::pixel_local;
    case AdjustmentOperation::selective_tone:
    case AdjustmentOperation::sharpen:
    case AdjustmentOperation::spot_heal:
        return AdjustmentLocality::neighborhood;
    }
    return AdjustmentLocality::pixel_local;
}

AdjustmentLocality locality(const AdjustmentParameters& parameters) noexcept {
    return std::visit(
        [](const auto& value) {
            using Parameters = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                return AdjustmentLocality::neighborhood;
            } else if constexpr (std::is_same_v<Parameters, SpotHealAdjustment>) {
                return AdjustmentLocality::neighborhood;
            } else if constexpr (std::is_same_v<Parameters, SharpenAdjustment>) {
                return value.execution_pass == DetailEffectsExecutionPass::technical_detail
                               || (value.execution_pass == DetailEffectsExecutionPass::color_grading
                                   && (value.clarity != 0.0 || value.texture != 0.0
                                       || value.local_contrast != 0.0))
                           ? AdjustmentLocality::neighborhood
                           : AdjustmentLocality::pixel_local;
            } else {
                return AdjustmentLocality::pixel_local;
            }
        },
        parameters
    );
}

AdjustmentFootprint footprint(
    const AdjustmentParameters& parameters,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y,
    const Dimensions raster_dimensions
) {
    if (!std::isfinite(level_zero_to_raster_scale_x) || level_zero_to_raster_scale_x <= 0.0
        || !std::isfinite(level_zero_to_raster_scale_y) || level_zero_to_raster_scale_y <= 0.0) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "adjustment footprint scales must be finite and positive"
        );
    }
    return std::visit(
        [level_zero_to_raster_scale_x,
         level_zero_to_raster_scale_y,
         raster_dimensions](const auto& value) {
            using Parameters = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                return prepare_guided_selective_tone(
                           value,
                           level_zero_to_raster_scale_x,
                           level_zero_to_raster_scale_y
                )
                    .footprint();
            } else if constexpr (std::is_same_v<Parameters, SpotHealAdjustment>) {
                validate_spot_heal(value);
                const detail::RetouchSourceReach reach = detail::retouch_source_reach(
                    value,
                    level_zero_to_raster_scale_x,
                    level_zero_to_raster_scale_y,
                    raster_dimensions
                );
                const double horizontal = std::ceil(reach.horizontal);
                const double vertical = std::ceil(reach.vertical);
                if (horizontal > std::numeric_limits<std::uint32_t>::max()
                    || vertical > std::numeric_limits<std::uint32_t>::max()) {
                    throw EditError(
                        EditErrorCode::numeric_overflow,
                        std::nullopt,
                        "spot-heal adjustment footprint exceeds the supported integer range"
                    );
                }
                return AdjustmentFootprint{
                    .horizontal_radius = static_cast<std::uint32_t>(horizontal),
                    .vertical_radius = static_cast<std::uint32_t>(vertical),
                };
            } else if constexpr (std::is_same_v<Parameters, SharpenAdjustment>) {
                if (value.execution_pass != DetailEffectsExecutionPass::technical_detail) {
                    if (value.execution_pass == DetailEffectsExecutionPass::color_grading) {
                        return creative_detail_footprint(
                            value,
                            level_zero_to_raster_scale_x,
                            level_zero_to_raster_scale_y
                        );
                    }
                    if (value.execution_pass == DetailEffectsExecutionPass::finishing_effects) {
                        return AdjustmentFootprint{};
                    }
                    throw EditError(
                        EditErrorCode::invalid_parameter,
                        std::nullopt,
                        "cannot calculate a footprint for an unknown Detail & Effects pass"
                    );
                }
                return technical_detail_footprint(
                    value,
                    level_zero_to_raster_scale_x,
                    level_zero_to_raster_scale_y
                );
            } else {
                return AdjustmentFootprint{};
            }
        },
        parameters
    );
}

void validate_adjustment_nodes(const std::span<const AdjustmentNode> nodes) {
    static_cast<void>(prepare_adjustment_nodes(nodes));
}

EditExecutionPlan compile_edit_execution_plan(
    const std::span<const AdjustmentNode> nodes,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y
) {
    // Validation is deliberately complete and precedes enabled/neutral filtering or any
    // scheduling-specific checks.
    const auto prepared_nodes = prepare_adjustment_nodes(nodes);
    // Validate scale arguments even when the recipe is empty or all nodes are omitted.
    static_cast<void>(footprint(
        AdjustmentParameters{ExposureAdjustment{}},
        level_zero_to_raster_scale_x,
        level_zero_to_raster_scale_y
    ));

    EditExecutionPlan plan{
        .source_node_count = nodes.size(),
    };
    const auto add_footprint = [&nodes](
                                   AdjustmentFootprint& destination,
                                   const AdjustmentFootprint addition,
                                   const std::size_t node_index
                               ) {
        if (addition.horizontal_radius
                > std::numeric_limits<std::uint32_t>::max() - destination.horizontal_radius
            || addition.vertical_radius
                   > std::numeric_limits<std::uint32_t>::max() - destination.vertical_radius) {
            throw_node_error(
                EditErrorCode::numeric_overflow,
                node_index,
                nodes[node_index],
                "compiled adjustment footprint exceeds the supported integer range"
            );
        }
        destination.horizontal_radius += addition.horizontal_radius;
        destination.vertical_radius += addition.vertical_radius;
    };

    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const AdjustmentNode& node = nodes[index];
        if (!node.enabled || adjustment_is_neutral(node.parameters, prepared_nodes[index])) {
            continue;
        }

        const AdjustmentLocality node_locality = locality(node.parameters);
        if (plan.segments.empty() || plan.segments.back().locality != node_locality) {
            plan.segments.push_back(
                EditExecutionSegment{
                    .locality = node_locality,
                    .first_node_index = index,
                    .past_last_node_index = index + 1U,
                }
            );
        }
        EditExecutionSegment& segment = plan.segments.back();
        segment.past_last_node_index = index + 1U;
        segment.steps.push_back(
            EditExecutionStep{
                .node_index = index,
                .operation = operation(node.parameters),
            }
        );

        AdjustmentFootprint node_footprint;
        try {
            node_footprint = footprint(
                node.parameters,
                level_zero_to_raster_scale_x,
                level_zero_to_raster_scale_y
            );
        } catch (const EditError& error) {
            throw_node_error(error.code(), index, node, error.what());
        }
        add_footprint(segment.cumulative_footprint, node_footprint, index);
        add_footprint(plan.cumulative_footprint, node_footprint, index);
    }
    return plan;
}

FloatRgbImage execute_adjustment_nodes(
    const FloatRgbImage& input,
    const std::span<const AdjustmentNode> nodes,
    AdjustmentExecutionContext context
) {
    validate_edit_image(input);
    const auto prepared_nodes = prepare_adjustment_nodes(nodes);
    context = validate_edit_execution_context(input, context);
    FloatRgbImage output = input;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (nodes[index].enabled) {
            apply_node(output, nodes[index], index, prepared_nodes[index], context);
        }
    }
    return output;
}

} // namespace shadow::image
