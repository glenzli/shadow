#include <shadow/image/edit.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

namespace shadow::image {

namespace {

static_assert(sizeof(float) == 4U);
static_assert(std::numeric_limits<float>::is_iec559);

constexpr std::size_t rgb_channels = 3U;

[[nodiscard]] std::string node_prefix(
    const std::size_t index,
    const AdjustmentNode& node
) {
    std::ostringstream message;
    message << "edit node " << index;
    if (!node.node_id.empty()) {
        message << " (" << node.node_id << ')';
    }
    message << ": ";
    return message.str();
}

[[noreturn]] void throw_node_error(
    const EditErrorCode code,
    const std::size_t index,
    const AdjustmentNode& node,
    const std::string_view detail
) {
    throw EditError(code, index, node_prefix(index, node) + std::string(detail));
}

[[nodiscard]] bool finite_chromaticity(const Chromaticity value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

void validate_image(const FloatRgbImage& image) {
    if (image.dimensions.width == 0U || image.dimensions.height == 0U) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input dimensions must be non-zero"
        );
    }
    if (image.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input must use native interleaved RGB float32 samples"
        );
    }
    if (
        image.transfer_function != TransferFunction::linear
        || image.reference != ImageReference::scene_referred
    ) {
        throw EditError(
            EditErrorCode::incompatible_color_encoding,
            std::nullopt,
            "edit input must be scene-referred linear RGB"
        );
    }

    const auto& space = image.working_space;
    if (
        space.id.empty() || !finite_chromaticity(space.white_point)
        || !std::ranges::all_of(space.primaries, finite_chromaticity)
        || !std::ranges::all_of(space.luminance_coefficients, [](const double value) {
               return std::isfinite(value);
           })
    ) {
        throw EditError(
            EditErrorCode::invalid_working_space,
            std::nullopt,
            "edit input requires a finite, explicitly identified RGB working space"
        );
    }
    const double luminance_sum =
        space.luminance_coefficients[0] + space.luminance_coefficients[1]
        + space.luminance_coefficients[2];
    if (std::abs(luminance_sum - 1.0) > 1.0e-6) {
        throw EditError(
            EditErrorCode::invalid_working_space,
            std::nullopt,
            "working-space luminance coefficients must sum to one"
        );
    }

    const std::uint64_t minimum_row_samples =
        static_cast<std::uint64_t>(image.dimensions.width) * rgb_channels;
    if (
        minimum_row_samples > std::numeric_limits<std::size_t>::max() / sizeof(float)
    ) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input row size overflows the address space"
        );
    }
    const std::size_t minimum_stride =
        static_cast<std::size_t>(minimum_row_samples) * sizeof(float);
    if (
        image.row_stride_bytes < minimum_stride
        || image.row_stride_bytes % sizeof(float) != 0U
    ) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input row stride is invalid"
        );
    }
    const std::size_t row_stride = image.row_stride_bytes / sizeof(float);
    const std::size_t height = static_cast<std::size_t>(image.dimensions.height);
    if (row_stride > std::numeric_limits<std::size_t>::max() / height) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input sample count overflows the address space"
        );
    }
    const std::size_t required_samples = row_stride * height;
    if (image.samples.size() != required_samples) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input sample storage must exactly match dimensions and stride"
        );
    }
    if (!std::ranges::all_of(image.samples, [](const float value) {
            return std::isfinite(value);
        })) {
        throw EditError(
            EditErrorCode::non_finite_value,
            std::nullopt,
            "edit input contains NaN or infinity"
        );
    }
}

void validate_node(const AdjustmentNode& node, const std::size_t index) {
    if (
        node.parameter_schema_version != adjustment_parameter_schema_version
        || node.implementation_version != adjustment_implementation_version
    ) {
        throw_node_error(
            EditErrorCode::unsupported_version,
            index,
            node,
            "only parameter schema 1 and implementation 1 are supported"
        );
    }

    std::visit(
        [&node, index](const auto& parameters) {
            using Parameters = std::decay_t<decltype(parameters)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
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
                if (
                    !std::isfinite(parameters.factor) || parameters.factor < 0.0
                    || !std::isfinite(parameters.pivot) || parameters.pivot < 0.0
                ) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "contrast factor and pivot must be finite and non-negative"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, ChannelGainAdjustment>) {
                if (!std::ranges::all_of(parameters.channel_gains, [](const double gain) {
                        return std::isfinite(gain) && gain > 0.0;
                    })) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "channel gains must be finite and positive"
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
            }
        },
        node.parameters
    );
}

[[nodiscard]] float checked_float(
    const double value,
    const std::size_t node_index,
    const AdjustmentNode& node
) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) {
        throw_node_error(
            EditErrorCode::numeric_overflow,
            node_index,
            node,
            "pixel result exceeded finite float32 range"
        );
    }
    return static_cast<float>(value);
}

template <typename Transform>
void transform_rgb_pixels(
    FloatRgbImage& image,
    const std::size_t node_index,
    const AdjustmentNode& node,
    Transform&& transform
) {
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    for (std::uint32_t y = 0; y < image.dimensions.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < image.dimensions.width; ++x) {
            const std::size_t sample = row + static_cast<std::size_t>(x) * rgb_channels;
            const std::array<double, 3> input{
                static_cast<double>(image.samples[sample]),
                static_cast<double>(image.samples[sample + 1U]),
                static_cast<double>(image.samples[sample + 2U]),
            };
            const std::array<double, 3> output = transform(input);
            image.samples[sample] = checked_float(output[0], node_index, node);
            image.samples[sample + 1U] = checked_float(output[1], node_index, node);
            image.samples[sample + 2U] = checked_float(output[2], node_index, node);
        }
    }
}

void apply_node(FloatRgbImage& image, const AdjustmentNode& node, const std::size_t index) {
    std::visit(
        [&image, &node, index](const auto& parameters) {
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
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters](const std::array<double, 3>& input) {
                        const auto adjust = [&parameters](const double value) {
                            return parameters.pivot
                                + (value - parameters.pivot) * parameters.factor;
                        };
                        return std::array{adjust(input[0]), adjust(input[1]), adjust(input[2])};
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, ChannelGainAdjustment>) {
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters](const std::array<double, 3>& input) {
                        return std::array{
                            input[0] * parameters.channel_gains[0],
                            input[1] * parameters.channel_gains[1],
                            input[2] * parameters.channel_gains[2],
                        };
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, SaturationAdjustment>) {
                const auto weights = image.working_space.luminance_coefficients;
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters, weights](const std::array<double, 3>& input) {
                        const double luminance = input[0] * weights[0]
                            + input[1] * weights[1] + input[2] * weights[2];
                        const auto adjust = [&parameters, luminance](const double value) {
                            return luminance + parameters.factor * (value - luminance);
                        };
                        return std::array{adjust(input[0]), adjust(input[1]), adjust(input[2])};
                    }
                );
            }
        },
        node.parameters
    );
}

} // namespace

EditError::EditError(
    const EditErrorCode code,
    const std::optional<std::size_t> node_index,
    std::string message
)
    : std::runtime_error(std::move(message)), code_(code), node_index_(node_index) {}

EditErrorCode EditError::code() const noexcept {
    return code_;
}

std::optional<std::size_t> EditError::node_index() const noexcept {
    return node_index_;
}

AdjustmentOperation operation(const AdjustmentParameters& parameters) noexcept {
    return std::visit(
        [](const auto& value) {
            using Parameters = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
                return AdjustmentOperation::exposure;
            } else if constexpr (std::is_same_v<Parameters, ContrastAdjustment>) {
                return AdjustmentOperation::contrast;
            } else if constexpr (std::is_same_v<Parameters, ChannelGainAdjustment>) {
                return AdjustmentOperation::channel_gain;
            } else {
                return AdjustmentOperation::saturation;
            }
        },
        parameters
    );
}

std::string_view operation_id(const AdjustmentOperation operation) noexcept {
    switch (operation) {
    case AdjustmentOperation::exposure:
        return "shadow.exposure";
    case AdjustmentOperation::contrast:
        return "shadow.contrast";
    case AdjustmentOperation::channel_gain:
        return "shadow.channel_gain";
    case AdjustmentOperation::saturation:
        return "shadow.saturation";
    }
    return "shadow.unknown";
}

FloatRgbImage execute_adjustment_nodes(
    const FloatRgbImage& input,
    const std::span<const AdjustmentNode> nodes
) {
    validate_image(input);
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        validate_node(nodes[index], index);
    }

    FloatRgbImage output = input;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (nodes[index].enabled) {
            apply_node(output, nodes[index], index);
        }
    }
    return output;
}

} // namespace shadow::image
