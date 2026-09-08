#include <shadow/image/lut_baking.hpp>

#include <shadow/image/color_management.hpp>
#include <shadow/image/cpu_edit_reference.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace shadow::image {
namespace {

constexpr std::size_t maximum_layers = 16U;
constexpr std::size_t chunk_pixels = 2'048U;
constexpr std::uint32_t validation_probes = 4'096U;

FloatRgbImage linear_image(Dimensions dimensions, std::vector<float> samples) {
    return {
        .dimensions = dimensions,
        .row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float),
        .pixel_format = FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = TransferFunction::linear,
        .reference = ImageReference::display_referred,
        .working_space = {
            .id = "srgb-d65-linear",
            .primaries = {Chromaticity{0.6400, 0.3300}, Chromaticity{0.3000, 0.6000},
                          Chromaticity{0.1500, 0.0600}},
            .white_point = {0.3127, 0.3290},
            .luminance_coefficients = {0.2126, 0.7152, 0.0722},
        },
        .samples = std::move(samples),
    };
}

void check_cancelled(std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        throw std::runtime_error("LUT baking cancelled");
    }
}

void validate_layers(std::span<const AdjustmentLayer> layers) {
    if (layers.empty() || layers.size() > maximum_layers) {
        throw std::invalid_argument("LUT baking requires 1 through 16 Grade Nodes");
    }
    for (const auto& layer : layers) {
        validate_adjustment_nodes(layer.nodes);
        if (!std::isfinite(layer.opacity) || layer.opacity < 0.0 || layer.opacity > 1.0) {
            throw std::invalid_argument("invalid LUT Grade Node strength");
        }
        if (!layer.enabled || layer.opacity == 0.0) {
            continue;
        }
        if (layer.mask) {
            throw std::invalid_argument("LUT baking cannot represent a masked Grade Node");
        }
        for (const auto& node : layer.nodes) {
            if (!node.enabled) {
                continue;
            }
            const bool supported = std::visit([](const auto& parameters) {
                using T = std::decay_t<decltype(parameters)>;
                if constexpr (std::is_same_v<T, SharpenAdjustment>) {
                    return parameters.execution_pass == DetailEffectsExecutionPass::color_grading
                        && parameters.clarity == 0.0 && parameters.texture == 0.0
                        && parameters.local_contrast == 0.0;
                } else {
                    return std::is_same_v<T, ExposureAdjustment>
                        || std::is_same_v<T, ContrastAdjustment>
                        || std::is_same_v<T, RgbWhiteBalanceAdjustment>
                        || std::is_same_v<T, SaturationAdjustment>
                        || std::is_same_v<T, OklabLightnessToneCurve>
                        || std::is_same_v<T, OklabOpponentToneCurves>
                        || std::is_same_v<T, PerceptualColorAdjustment>
                        || std::is_same_v<T, OklabColorWarperAdjustment>
                        || std::is_same_v<T, CubeLutAdjustment>;
                }
            }, node.parameters);
            if (!supported) {
                throw std::invalid_argument("LUT baking cannot represent this operation: " + node.node_id);
            }
        }
    }
}

std::vector<float> evaluate(
    std::span<const AdjustmentLayer> layers,
    std::span<const float> inputs,
    std::stop_token cancellation
) {
    std::vector<float> result;
    result.reserve(inputs.size());
    for (std::size_t offset = 0; offset < inputs.size(); offset += chunk_pixels * 3U) {
        check_cancelled(cancellation);
        const std::size_t count = std::min(chunk_pixels * 3U, inputs.size() - offset);
        auto image = linear_image(
            {static_cast<std::uint32_t>(count / 3U), 1U},
            {inputs.begin() + static_cast<std::ptrdiff_t>(offset),
             inputs.begin() + static_cast<std::ptrdiff_t>(offset + count)}
        );
        for (const auto& layer : layers) {
            check_cancelled(cancellation);
            image = execute_adjustment_layers(image, std::span{&layer, 1U});
        }
        result.insert(result.end(), image.samples.begin(), image.samples.end());
    }
    return result;
}

float radical_inverse(std::uint32_t index, std::uint32_t base) {
    double value = 0.0;
    double fraction = 1.0 / static_cast<double>(base);
    while (index > 0U) {
        value += static_cast<double>(index % base) * fraction;
        index /= base;
        fraction /= static_cast<double>(base);
    }
    return static_cast<float>(value);
}

} // namespace

CubeLutBakeResult bake_cube_lut(
    std::span<const AdjustmentLayer> layers,
    std::uint16_t size,
    std::stop_token cancellation
) {
    check_cancelled(cancellation);
    validate_layers(layers);
    if (size != 17U && size != 33U && size != 65U) {
        throw std::invalid_argument("LUT baking size must be 17, 33, or 65");
    }
    std::vector<float> inputs;
    inputs.reserve(static_cast<std::size_t>(size) * size * size * 3U);
    for (std::uint16_t blue = 0; blue < size; ++blue) {
        for (std::uint16_t green = 0; green < size; ++green) {
            for (std::uint16_t red = 0; red < size; ++red) {
                for (const auto channel : {red, green, blue}) {
                    inputs.push_back(static_cast<float>(channel) / static_cast<float>(size - 1U));
                }
            }
        }
    }
    const auto output = evaluate(layers, inputs, cancellation);
    CubeLutBakeResult result{.lut = {.title = "Shadow linear sRGB grade", .size = size}};
    result.lut.entries.reserve(output.size() / 3U);
    for (std::size_t offset = 0; offset < output.size(); offset += 3U) {
        result.lut.entries.push_back({output[offset], output[offset + 1U], output[offset + 2U]});
    }

    inputs.clear();
    for (std::uint32_t index = 1U; index <= validation_probes; ++index) {
        for (const auto base : {2U, 3U, 5U}) {
            inputs.push_back(radical_inverse(index, base));
        }
    }
    const auto reference = evaluate(layers, inputs, cancellation);
    double squared_error = 0.0;
    for (std::size_t offset = 0; offset < inputs.size(); offset += 3U) {
        const auto sampled = sample_cube_lut(
            result.lut, {inputs[offset], inputs[offset + 1U], inputs[offset + 2U]}
        );
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            const double error = std::abs(static_cast<double>(sampled[channel]) - reference[offset + channel]);
            result.maximum_absolute_error = std::max(result.maximum_absolute_error, error);
            squared_error += error * error;
        }
    }
    result.root_mean_square_error = std::sqrt(squared_error / static_cast<double>(inputs.size()));
    result.probe_count = validation_probes;
    check_cancelled(cancellation);
    return result;
}

std::string serialize_baked_cube_lut(const CubeLutBakeResult& baked) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(9)
         << "# Shadow LUT bake v1\n# Input: linear sRGB / D65\n# Output: linear sRGB / D65\n"
         << "# Input domain: [0, 1]; outside-domain colors are not represented.\n"
         << "# RAW development and display/output rendering are not included.\n"
         << "# Measured max RGB error: " << baked.maximum_absolute_error
         << "\n# Measured RMS RGB error: " << baked.root_mean_square_error
         << "\n# Independent probe count: " << baked.probe_count
         << "\nTITLE \"Shadow linear sRGB grade\"\nLUT_3D_SIZE " << baked.lut.size
         << "\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    for (const auto& entry : baked.lut.entries) {
        text << entry[0] << ' ' << entry[1] << ' ' << entry[2] << '\n';
    }
    auto document = text.str();
    static_cast<void>(parse_cube_lut(document));
    return document;
}

DisplayRgb8Image render_cube_lut_reference(
    const CubeLut3D& lut,
    Dimensions dimensions,
    std::span<const float> encoded_srgb
) {
    if (dimensions.width == 0U || dimensions.height == 0U
        || dimensions.width > 512U || dimensions.height > 512U
        || encoded_srgb.size() != static_cast<std::size_t>(dimensions.width) * dimensions.height * 3U) {
        throw std::invalid_argument("invalid bounded LUT reference image");
    }
    auto source = linear_image(dimensions, {encoded_srgb.begin(), encoded_srgb.end()});
    make_icc_transform(make_display_srgb_icc_profile(), make_linear_srgb_icc_profile())
        .apply_interleaved_rgb(source.samples);
    const AdjustmentNode node{
        .node_id = "lut-library-reference",
        .parameters = CubeLutAdjustment{.lut = lut, .intensity = 1.0},
    };
    const auto edited = execute_adjustment_nodes(source, std::span{&node, 1U});
    return render_linear_srgb_to_display_srgb8_cpu_reference(
        edited, {.target_dimensions = dimensions}
    );
}

} // namespace shadow::image
