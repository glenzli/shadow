#pragma once

#include <shadow/image/decoder.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace shadow::image {

// This is an in-process CPU buffer, not a persistence or public ABI format. Samples are
// native-endian IEEE-754 binary32 values in interleaved R, G, B order.
enum class FloatPixelFormat : std::uint8_t {
    unknown,
    rgb_f32_native_interleaved,
};

enum class TransferFunction : std::uint8_t {
    unknown,
    linear,
};

enum class ImageReference : std::uint8_t {
    unknown,
    scene_referred,
};

struct Chromaticity final {
    double x = 0.0;
    double y = 0.0;

    auto operator<=>(const Chromaticity&) const = default;
};

// The luminance coefficients are the Y row of the working-RGB-to-XYZ matrix. Keeping
// them beside the primaries makes saturation independent of any hard-coded working space.
struct WorkingRgbSpace final {
    std::string id;
    std::array<Chromaticity, 3> primaries{};
    Chromaticity white_point;
    std::array<double, 3> luminance_coefficients{};

    auto operator<=>(const WorkingRgbSpace&) const = default;
};

struct FloatRgbImage final {
    Dimensions dimensions;
    std::size_t row_stride_bytes = 0;
    FloatPixelFormat pixel_format = FloatPixelFormat::unknown;
    TransferFunction transfer_function = TransferFunction::unknown;
    ImageReference reference = ImageReference::unknown;
    WorkingRgbSpace working_space;
    std::vector<float> samples;
};

struct ExposureAdjustment final {
    // Linear-light gain is 2^stops. No highlight clipping is performed.
    double stops = 0.0;
};

struct ContrastAdjustment final {
    // output = pivot + (input - pivot) * factor. factor=1 is neutral.
    double factor = 1.0;
    double pivot = 0.18;
};

struct ChannelGainAdjustment final {
    // Positive scene-linear R, G, B multipliers. This is deliberately not named white
    // balance: camera-domain WB and chromatic adaptation need richer color semantics.
    std::array<double, 3> channel_gains{1.0, 1.0, 1.0};
};

struct SaturationAdjustment final {
    // Luma-preserving linear-RGB interpolation. factor=0 is monochrome, 1 is neutral.
    double factor = 1.0;
};

using AdjustmentParameters = std::variant<
    ExposureAdjustment,
    ContrastAdjustment,
    ChannelGainAdjustment,
    SaturationAdjustment>;

enum class AdjustmentOperation : std::uint8_t {
    exposure,
    contrast,
    channel_gain,
    saturation,
};

inline constexpr std::uint32_t adjustment_parameter_schema_version = 1;
inline constexpr std::uint32_t adjustment_implementation_version = 1;

struct AdjustmentNode final {
    std::string node_id;
    std::uint32_t parameter_schema_version = adjustment_parameter_schema_version;
    std::uint32_t implementation_version = adjustment_implementation_version;
    bool enabled = true;
    AdjustmentParameters parameters = ExposureAdjustment{};
};

enum class EditErrorCode : std::uint8_t {
    invalid_image_layout,
    incompatible_color_encoding,
    invalid_working_space,
    invalid_parameter,
    unsupported_version,
    non_finite_value,
    numeric_overflow,
};

class EditError final : public std::runtime_error {
public:
    EditError(
        EditErrorCode code,
        std::optional<std::size_t> node_index,
        std::string message
    );

    [[nodiscard]] EditErrorCode code() const noexcept;
    [[nodiscard]] std::optional<std::size_t> node_index() const noexcept;

private:
    EditErrorCode code_;
    std::optional<std::size_t> node_index_;
};

[[nodiscard]] AdjustmentOperation operation(const AdjustmentParameters& parameters) noexcept;
[[nodiscard]] std::string_view operation_id(AdjustmentOperation operation) noexcept;

// Executes an intentionally linear subset of the future typed edit graph. Nodes are applied
// in span order, disabled nodes are skipped, and the input is never mutated. The executor does
// not clamp negative or >1 values and rejects NaN/Inf rather than silently contaminating caches.
[[nodiscard]] FloatRgbImage execute_adjustment_nodes(
    const FloatRgbImage& input,
    std::span<const AdjustmentNode> nodes
);

// Renders a standard display-referred JPEG while keeping adjustment math in explicitly
// scene-referred linear sRGB. The LibRaw reference buffer is decoded from its sRGB transfer
// function before node execution and encoded back to sRGB only at the output boundary.
[[nodiscard]] EncodedProxy render_edited_reference_proxy_jpeg(
    const DecodeSession& session,
    std::span<const AdjustmentNode> nodes,
    ProxyRequest request = {}
);

} // namespace shadow::image
