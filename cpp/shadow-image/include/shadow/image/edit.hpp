#pragma once

#include <shadow/image/lut.hpp>

#include <shadow/image/optics.hpp>

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
    // Relative processed scene-referred RGB: values remain linear-light and no display OETF or
    // look/tone rendering has been applied, but camera black subtraction, white balance,
    // demosaic, color-matrix conversion, normalization, and highlight clipping may already have
    // occurred. This must not be interpreted as sensor-linear mosaic/radiance data.
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
    // The raster's sampling density relative to level-0/full-resolution pixels. A full-detail
    // image is 1x1; a 1/4-size warm proxy is approximately 0.25x0.25. Spatial operations use
    // these values to keep their public radius expressed in level-0 pixels.
    double level_zero_to_raster_scale_x = 1.0;
    double level_zero_to_raster_scale_y = 1.0;
    std::vector<float> samples;
};

struct ExposureAdjustment final {
    // Linear-light gain is 2^stops. No highlight clipping is performed.
    double stops = 0.0;
};

struct ContrastAdjustment final {
    // A scene-linear, luminance-preserving S curve centered at pivot. factor=1 is neutral.
    // Unlike an affine per-channel scale, this keeps the toe and shoulder finite and retains
    // chromatic ratios instead of producing negative colour channels in deep shadows.
    double factor = 1.0;
    double pivot = 0.18;
};

struct RgbWhiteBalanceAdjustment final {
    // Post-demosaic creative white balance in the declared D65 working space.
    // Temperature and tint are normalized user intent in [-1, 1]. Positive
    // temperature warms the image; positive tint moves away from green toward
    // magenta. This is deliberately distinct from sensor-domain RAW WB.
    double temperature = 0.0;
    double tint = 0.0;
};

struct SaturationAdjustment final {
    // Luma-preserving linear-RGB interpolation. factor=0 is monochrome, 1 is neutral.
    double factor = 1.0;
};

// Scene-referred regional tone controls expressed as bounded, implementation-independent
// amounts. Their implementation deliberately distinguishes the endpoints (Blacks/Whites)
// from the broad recovery ranges (Shadows/Highlights): endpoint controls shape a gentle
// toe/shoulder response while recovery controls apply a wider EV-domain exposure field.
//
// This is intentionally not a clone of any particular RAW developer. It is Shadow's compact,
// ratio-preserving baseline that later local-masking implementations can refine without
// changing the public control vocabulary. Zeroes are exactly neutral.
struct SelectiveToneAdjustment final {
    double highlights = 0.0;
    double shadows = 0.0;
    double whites = 0.0;
    double blacks = 0.0;
};

inline constexpr std::size_t perceptual_hue_band_count = 8U;
inline constexpr std::size_t maximum_point_color_ranges = 16U;
inline constexpr std::uint32_t perceptual_color_v2_parameter_schema_version = 2;
inline constexpr std::uint32_t perceptual_color_v2_implementation_version = 2;

// Optional circular hue selection evaluated against the source Oklch hue. width_degrees is the
// half-width of the selected range; softness is the fraction of that half-width used as a smooth
// edge. Hue distance is circular, so a range centered at zero naturally spans the 0/360 seam.
struct PerceptualColorRange final {
    bool enabled = false;
    double center_degrees = 0.0;
    double width_degrees = 30.0;
    double softness = 0.5;
    double hue_shift_degrees = 0.0;
    double saturation = 0.0;
    double lightness = 0.0;
};

// Perceptual color controls evaluated in Oklab/Oklch. The public band order is red, orange,
// yellow, green, aqua, blue, purple, magenta. Implementation-version 1 anchors those names at
// the non-uniform Oklch hues of representative linear-sRGB colors and uses a smooth periodic
// partition of unity between adjacent anchors; it must never be interpreted as an HSV wheel.
// hue values in [-1, 1] map to [-30, 30] degrees; saturation/lightness and vibrance are
// normalized amounts in [-1, 1]. Achromatic pixels are deliberately left unchanged because
// hue is undefined at low chroma.
struct PerceptualColorAdjustment final {
    double vibrance = 0.0;
    std::array<double, perceptual_hue_band_count> hue{};
    std::array<double, perceptual_hue_band_count> saturation{};
    std::array<double, perceptual_hue_band_count> lightness{};
    PerceptualColorRange color_range;
    std::vector<PerceptualColorRange> additional_color_ranges;
};

// Immutable 3D `.cube` resource applied in processed working RGB. Intensity
// linearly blends the sampled result with the node input; zero is an exact
// no-op and permits an empty LUT for a stable, unselected Recipe slot.
struct CubeLutAdjustment final {
    CubeLut3D lut;
    double intensity = 0.0;
};

// Luminance-only unsharp masking in scene-linear RGB. radius is the level-0 Gaussian sigma;
// threshold maps linearly to at most 0.25 EV of soft-thresholding. A common gain is applied to
// R, G, and B so sharpening cannot introduce chromatic fringes by treating channels separately.
struct SharpenAdjustment final {
    double amount = 0.0;
    double radius = 1.0;
    double threshold = 0.0;
    double masking = 0.0;
    double denoise_luminance = 0.0;
    double denoise_detail = 0.5;
    double denoise_color = 0.0;
    double dehaze = 0.0;
    double defringe_purple_amount = 0.0;
    double defringe_purple_hue_low = 270.0;
    double defringe_purple_hue_high = 340.0;
    double defringe_green_amount = 0.0;
    double defringe_green_hue_low = 100.0;
    double defringe_green_hue_high = 165.0;
    double shadows_hue = 0.0;
    double shadows_saturation = 0.0;
    double shadows_luminance = 0.0;
    double midtones_hue = 0.0;
    double midtones_saturation = 0.0;
    double midtones_luminance = 0.0;
    double highlights_hue = 0.0;
    double highlights_saturation = 0.0;
    double highlights_luminance = 0.0;
    double grading_blending = 0.5;
    double grading_balance = 0.0;
    double grain_amount = 0.0;
    double grain_size = 0.5;
    double grain_roughness = 0.5;
    double vignette_amount = 0.0;
    double vignette_midpoint = 0.5;
    double vignette_roundness = 0.0;
    double vignette_feather = 0.5;
    double vignette_highlights = 0.0;
};

inline constexpr std::uint32_t detail_effects_v2_parameter_schema_version = 2;
inline constexpr std::uint32_t detail_effects_v2_implementation_version = 2;

inline constexpr std::uint32_t tone_curve_parameter_schema_version = 1;
inline constexpr std::uint32_t tone_curve_implementation_version = 1;
inline constexpr std::uint32_t smooth_rgb_tone_curve_parameter_schema_version = 2;
inline constexpr std::uint32_t smooth_rgb_tone_curve_implementation_version = 2;
// Per individual curve, for both the legacy curve and each v2 master/channel set.
inline constexpr std::size_t maximum_tone_curve_points = 256U;
inline constexpr std::size_t maximum_tone_curve_preview_samples = 4'097U;

struct ToneCurvePoint final {
    double x = 0.0;
    double y = 0.0;

    auto operator<=>(const ToneCurvePoint&) const = default;
};

// Version 1 is an intentionally simple, deterministic reference curve. Control-point x
// coordinates span the normalized [0, 1] domain; y remains unbounded so lifted blacks and
// super-white results are representable. The default two-point curve is exactly neutral.
struct ToneCurve final {
    std::uint32_t parameter_schema_version = tone_curve_parameter_schema_version;
    std::uint32_t implementation_version = tone_curve_implementation_version;
    std::vector<ToneCurvePoint> points{{0.0, 0.0}, {1.0, 1.0}};
};

// One set of interpolation knots for the version-2 RGB point-curve contract. The
// implementation fits a local, shape-preserving Fritsch-Butland PCHIP through these
// points. Strictly increasing x coordinates span [0, 1]; y remains unbounded.
struct ToneCurveSet final {
    std::vector<ToneCurvePoint> points{{0.0, 0.0}, {1.0, 1.0}};
};

// Version 2 groups the overall/master and three channel curves into one atomic
// adjustment. Each channel is evaluated as channel(master(input)); all four identity
// curves are an exact no-op. Negative and super-white inputs use linear endpoint-tangent
// extrapolation rather than clipping or extending a cubic polynomial beyond [0, 1].
struct SmoothRgbToneCurve final {
    std::uint32_t parameter_schema_version = smooth_rgb_tone_curve_parameter_schema_version;
    std::uint32_t implementation_version = smooth_rgb_tone_curve_implementation_version;
    ToneCurveSet master;
    ToneCurveSet red;
    ToneCurveSet green;
    ToneCurveSet blue;
};

using AdjustmentParameters = std::variant<
    ExposureAdjustment,
    ContrastAdjustment,
    ToneCurve,
    SmoothRgbToneCurve,
    RgbWhiteBalanceAdjustment,
    SaturationAdjustment,
    SelectiveToneAdjustment,
    PerceptualColorAdjustment,
    CubeLutAdjustment,
    SharpenAdjustment>;

enum class AdjustmentOperation : std::uint8_t {
    exposure,
    contrast,
    tone_curve,
    smooth_rgb_tone_curve,
    rgb_white_balance,
    saturation,
    selective_tone,
    perceptual_color,
    lut_3d,
    sharpen,
};

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

inline constexpr std::uint32_t adjustment_parameter_schema_version = 1;
inline constexpr std::uint32_t adjustment_implementation_version = 1;
// A square proxy at this limit occupies at most 192 MiB as interleaved RGB float32.
// Typical 3:2 photos at the UI's 1600/2048 edge use substantially less memory.
inline constexpr std::uint32_t maximum_warm_edit_preview_edge = 4'096;
// Full-detail sessions retain the provider's complete 16-bit reference RGB image, but never
// more than 512 MiB. The metadata preflight assumes worst-case RGB even when a provider may
// ultimately return one-channel grayscale data.
inline constexpr std::uint64_t maximum_full_edit_detail_retained_bytes = 512ULL * 1'024ULL * 1'024ULL;
// Detail work stays tile-local so one request cannot accidentally materialize another full-size
// float image while the immutable 16-bit source is resident.
inline constexpr std::uint32_t maximum_edit_detail_tile_side = 1'024;
inline constexpr std::uint32_t maximum_edit_detail_total_apron = 512;
inline constexpr std::uint32_t maximum_edit_detail_working_side = 2'048;

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
[[nodiscard]] AdjustmentLocality locality(AdjustmentOperation operation) noexcept;
// The supplied scales are level-0-to-raster sampling densities. Invalid/non-finite scales or
// malformed parameters fail closed; callers normally validate the full node plan first.
[[nodiscard]] AdjustmentFootprint footprint(
    const AdjustmentParameters& parameters,
    double level_zero_to_raster_scale_x = 1.0,
    double level_zero_to_raster_scale_y = 1.0
);

// Validates the complete adjustment plan without requiring image pixels. All nodes, including
// disabled ones, are checked for supported versions, finite parameters, and valid Tone Curve
// geometry/slopes. This lets callers reject malformed work before an expensive decode. Pixel-
// dependent overflow remains the responsibility of execute_adjustment_nodes().
void validate_adjustment_nodes(std::span<const AdjustmentNode> nodes);

// Global raster coordinates keep deterministic grain and radial effects identical between a
// full proxy and independently rendered detail tiles. Zero full dimensions mean "use input".
struct AdjustmentExecutionContext final {
    std::uint32_t origin_x = 0;
    std::uint32_t origin_y = 0;
    Dimensions full_dimensions{};
    // Robust image-level log-luminance key relative to 18% gray. The renderer supplies this for
    // RAW-backed preview and detail sessions so regional tone controls do not become ineffective
    // merely because a camera or exposure places its entire scene above/below a fixed numeric
    // zone. Zero preserves the standalone executor's canonical 18%-gray behavior.
    double selective_tone_scene_key_ev = 0.0;
};

// Executes an intentionally compact subset of the future typed edit graph. The recommended
// default pipeline order is Exposure -> Contrast -> SelectiveTone -> ToneCurve (legacy or
// SmoothRgbToneCurve) -> RgbWhiteBalance -> Saturation -> PerceptualColor, but that is a recipe
// convention: this executor always applies nodes in the supplied span order.
// Disabled nodes are skipped and the input is never mutated. The executor does not clamp
// negative or >1 values and rejects NaN/Inf rather than silently contaminating caches.
[[nodiscard]] FloatRgbImage execute_adjustment_nodes(
    const FloatRgbImage& input,
    std::span<const AdjustmentNode> nodes,
    AdjustmentExecutionContext context = {}
);

// Applies the same piecewise-linear curve independently to every channel in scene-linear
// working RGB. Samples in [0, 1] are interpolated between control points; negative and
// greater-than-one samples are linearly extrapolated with the first and last segment slopes.
// No clipping or implicit perceptual/luma conversion occurs. This is the version-1 CPU
// correctness baseline, not Shadow's final perceptual tone-curve design.
[[nodiscard]] FloatRgbImage apply_tone_curve(
    const FloatRgbImage& input,
    const ToneCurve& curve
);

// Applies the version-2 master/R/G/B curve set in processed linear-light working RGB.
// This is the standalone equivalent of a SmoothRgbToneCurve adjustment node.
[[nodiscard]] FloatRgbImage apply_smooth_rgb_tone_curve(
    const FloatRgbImage& input,
    const SmoothRgbToneCurve& curve
);

// Samples the exact version-2 core evaluator at uniformly spaced x coordinates in [0, 1].
// UI code should draw these samples instead of fitting an unrelated display-only Bezier.
// sample_count must be between 2 and maximum_tone_curve_preview_samples, inclusive.
[[nodiscard]] std::vector<ToneCurvePoint> sample_smooth_tone_curve(
    const ToneCurveSet& curve,
    std::size_t sample_count
);

// An immutable, reusable scene-linear working proxy for interactive editing. Preparation is
// the only operation that asks DecodeSession to render the RAW. render_jpeg() owns all of its
// temporary edit/JPEG state, so concurrent const calls are safe after construction.
//
// The version-1 operations are pixel-local transforms in scene-linear RGB. Linear/affine nodes
// commute with the bilinear downsampling used to prepare this proxy. ToneCurve is nonlinear, so
// applying it here is an interactive proxy approximation rather than a bit-equivalent substitute
// for applying it before full-resolution downsampling. Masked or neighborhood operations must
// still declare an appropriate preview strategy rather than being silently routed through here.
inline constexpr std::size_t edit_preview_histogram_bin_count = 256U;
inline constexpr std::string_view edit_preview_analysis_version =
    "shadow.edit-preview-analysis.v1:rgb8-before-jpeg:rec709-encoded-q16:"
    "pre-clamp-linear-strict-lt-gt-any-channel";

// Transient analysis of one complete warm-proxy render. Histogram bins describe the uncompressed
// display-sRGB RGB8 pixels immediately before JPEG encoding. Clipping counts inspect the edited
// scene-linear values immediately before output clamping: exact 0 and 1 are legal, while a pixel
// is counted when any channel is below 0 or above 1. This is not sensor-domain exposure analysis.
struct EditPreviewAnalysis final {
    Dimensions sample_dimensions;
    std::array<std::uint64_t, edit_preview_histogram_bin_count> red{};
    std::array<std::uint64_t, edit_preview_histogram_bin_count> green{};
    std::array<std::uint64_t, edit_preview_histogram_bin_count> blue{};
    std::array<std::uint64_t, edit_preview_histogram_bin_count> luma{};
    std::array<std::uint64_t, 3> below_zero_samples{};
    std::array<std::uint64_t, 3> above_one_samples{};
    std::uint64_t pixel_count = 0;
    std::uint64_t shadow_clipped_pixels = 0;
    std::uint64_t highlight_clipped_pixels = 0;

    auto operator<=>(const EditPreviewAnalysis&) const = default;
};

struct AnalyzedEditPreview final {
    EncodedProxy proxy;
    EditPreviewAnalysis analysis;
};

class WarmEditPreviewSession final {
public:
    WarmEditPreviewSession(const WarmEditPreviewSession&) = delete;
    WarmEditPreviewSession& operator=(const WarmEditPreviewSession&) = delete;
    WarmEditPreviewSession(WarmEditPreviewSession&&) noexcept = default;
    WarmEditPreviewSession& operator=(WarmEditPreviewSession&&) noexcept = default;
    ~WarmEditPreviewSession() = default;

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint32_t max_edge() const noexcept;
    [[nodiscard]] const OpticsProfileReceipt& optics_receipt() const noexcept;
    [[nodiscard]] EncodedProxy render_jpeg(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality = 88
    ) const;
    [[nodiscard]] AnalyzedEditPreview render_jpeg_with_analysis(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality = 88
    ) const;

private:
    WarmEditPreviewSession(
        FloatRgbImage working_proxy,
        std::uint32_t max_edge,
        OpticsProfileReceipt optics_receipt,
        double selective_tone_scene_key_ev
    );

    FloatRgbImage working_proxy_;
    std::uint32_t max_edge_ = 0;
    OpticsProfileReceipt optics_receipt_;
    double selective_tone_scene_key_ev_ = 0.0;

    friend WarmEditPreviewSession prepare_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge,
        const OpticsProvider* optics_provider,
        const OpticsSettings& optics_settings
    );
};

struct DetailTileRect final {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    auto operator<=>(const DetailTileRect&) const = default;
};

// Packed, display-referred sRGB bytes for one exact full-resolution rectangle. Rows carry no
// padding and no compression is applied, avoiding independently encoded JPEG block or chroma
// boundaries between neighboring tiles.
struct RenderedDetailTile final {
    DetailTileRect rect;
    Dimensions full_dimensions;
    std::uint32_t row_stride_bytes = 0;
    std::vector<std::uint8_t> bytes;
};

// An immutable complete processed-linear 16-bit image in sRGB primaries, used only for 1:1
// detail requests. No decoder survives preparation; each const render allocates and edits only
// the requested tile, which makes concurrent renders independent after construction.
class FullEditDetailSession final {
public:
    FullEditDetailSession(const FullEditDetailSession&) = delete;
    FullEditDetailSession& operator=(const FullEditDetailSession&) = delete;
    FullEditDetailSession(FullEditDetailSession&&) noexcept = default;
    FullEditDetailSession& operator=(FullEditDetailSession&&) noexcept = default;
    ~FullEditDetailSession() = default;

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] const OpticsProfileReceipt& optics_receipt() const noexcept;
    [[nodiscard]] RenderedDetailTile render_rgb8(
        std::span<const AdjustmentNode> nodes,
        DetailTileRect rect
    ) const;

private:
    FullEditDetailSession(
        PixelBuffer reference_rgb,
        std::uint64_t retained_bytes,
        OpticsProfileReceipt optics_receipt,
        double dng_baseline_exposure_stops,
        double selective_tone_scene_key_ev
    );

    PixelBuffer reference_rgb_;
    std::uint64_t retained_bytes_ = 0;
    OpticsProfileReceipt optics_receipt_;
    // A valid DNG BaselineExposure is part of the source rendering, rather than an editable
    // user node. Retain only the scalar so full-resolution data stays immutable and tiles apply
    // the same source appearance as the warm proxy immediately before the edit graph.
    double dng_baseline_exposure_stops_ = 0.0;
    double selective_tone_scene_key_ev_ = 0.0;

    friend FullEditDetailSession prepare_full_edit_detail(
        const DecodeSession& session,
        const OpticsProvider* optics_provider,
        const OpticsSettings& optics_settings
    );
};

// Decodes processed linear-light sRGB-primary u16 once and stores only a max-edge-bounded linear
// float proxy. Normalization precedes bilinear downsampling; no transfer is decoded and the
// full-size float image is never materialized.
[[nodiscard]] WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    std::uint32_t max_edge = 2'048,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// Checks the provider metadata against the worst-case RGB u16 retention bound before asking it
// to render pixels, then independently checks the actual retained vector allocation.
[[nodiscard]] FullEditDetailSession prepare_full_edit_detail(
    const DecodeSession& session,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// Renders a standard display-referred JPEG while keeping adjustment math in explicitly linear
// sRGB working RGB. LibRaw is configured to provide processed linear-light u16; the versioned
// output boundary gamut-maps and applies the sRGB transfer only after node execution.
[[nodiscard]] EncodedProxy render_edited_reference_proxy_jpeg(
    const DecodeSession& session,
    std::span<const AdjustmentNode> nodes,
    ProxyRequest request = {},
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

} // namespace shadow::image
