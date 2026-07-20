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
// A square proxy at this limit occupies at most 192 MiB as interleaved RGB float32.
// Typical 3:2 photos at the UI's 1600/2048 edge use substantially less memory.
inline constexpr std::uint32_t maximum_warm_edit_preview_edge = 4'096;

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

// An immutable, reusable scene-linear working proxy for interactive editing. Preparation is
// the only operation that asks DecodeSession to render the RAW. render_jpeg() owns all of its
// temporary edit/JPEG state, so concurrent const calls are safe after construction.
//
// The version-1 operations are pixel-local linear/affine transforms in scene-linear RGB.
// Therefore bilinear downsampling may happen before those operations without changing their
// mathematical result. Nonlinear, masked, or neighborhood operations must declare a different
// preview strategy rather than being silently routed through this class.
class WarmEditPreviewSession final {
public:
    WarmEditPreviewSession(const WarmEditPreviewSession&) = delete;
    WarmEditPreviewSession& operator=(const WarmEditPreviewSession&) = delete;
    WarmEditPreviewSession(WarmEditPreviewSession&&) noexcept = default;
    WarmEditPreviewSession& operator=(WarmEditPreviewSession&&) noexcept = default;
    ~WarmEditPreviewSession() = default;

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint32_t max_edge() const noexcept;
    [[nodiscard]] EncodedProxy render_jpeg(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality = 88
    ) const;

private:
    WarmEditPreviewSession(FloatRgbImage working_proxy, std::uint32_t max_edge);

    FloatRgbImage working_proxy_;
    std::uint32_t max_edge_ = 0;

    friend WarmEditPreviewSession prepare_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge
    );
};

// Decodes once and stores only a max-edge-bounded scene-linear sRGB float proxy. Conversion to
// linear light precedes bilinear downsampling; the full-size float image is never materialized.
[[nodiscard]] WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    std::uint32_t max_edge = 2'048
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
