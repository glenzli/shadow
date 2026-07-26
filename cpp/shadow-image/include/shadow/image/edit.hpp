#pragma once

#include <shadow/image/lut.hpp>

#include <shadow/image/optics.hpp>

#include <shadow/image/decoder.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/source_rendering.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
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
    // An ordinary rendered source (JPEG/SDR HEIF) that has been colour-managed and transfer
    // decoded into linear working RGB. It is linear for the purpose of composable adjustments,
    // but its original appearance is already display-referred; the output boundary therefore
    // must apply gamut mapping and the sRGB OETF only, rather than Shadow's RAW scene curve.
    display_referred,
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

/// Lossless right-angle orientation applied to the final photo canvas.
///
/// This is intentionally outside the adjustment-node enum: crop and
/// orientation alter output dimensions, while a node transforms samples in an
/// already-established raster. The source-coordinate Grade Node graph and
/// photo-local repair pass therefore execute before this state is applied.
enum class PhotoQuarterTurn : std::uint8_t {
    zero = 0U,
    clockwise_90 = 1U,
    clockwise_180 = 2U,
    clockwise_270 = 3U,
};

struct PhotoGeometry final {
    double crop_left = 0.0;
    double crop_top = 0.0;
    double crop_right = 1.0;
    double crop_bottom = 1.0;
    PhotoQuarterTurn quarter_turn = PhotoQuarterTurn::zero;
    // Fine rotation automatically narrows the final canvas to remove the
    // empty corners it would otherwise create, while retaining this crop's
    // aspect ratio.
    double straighten_degrees = 0.0;
    bool flip_horizontal = false;
    bool flip_vertical = false;

    auto operator<=>(const PhotoGeometry&) const = default;
};

struct GeometryPixelRect final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    auto operator<=>(const GeometryPixelRect&) const = default;
};

/// A validated integer crop and its final output dimensions. This one layout
/// is shared by complete warm-proxy execution and bounded full-detail tiles,
/// so their crop edges can never diverge due to independent rounding rules.
struct PhotoGeometryLayout final {
    GeometryPixelRect source_crop;
    Dimensions output_dimensions;

    auto operator<=>(const PhotoGeometryLayout&) const = default;
};

void validate_photo_geometry(const PhotoGeometry& geometry);
[[nodiscard]] PhotoGeometryLayout photo_geometry_layout(
    Dimensions source_dimensions,
    const PhotoGeometry& geometry
);
[[nodiscard]] GeometryPixelRect photo_geometry_source_rect_for_output(
    const PhotoGeometryLayout& layout,
    const PhotoGeometry& geometry,
    GeometryPixelRect output_rect
);
[[nodiscard]] FloatRgbImage apply_photo_geometry(
    const FloatRgbImage& source,
    const PhotoGeometry& geometry
);
[[nodiscard]] FloatRgbImage apply_photo_geometry_tile(
    const FloatRgbImage& source_tile,
    GeometryPixelRect source_tile_rect,
    const PhotoGeometryLayout& layout,
    const PhotoGeometry& geometry,
    GeometryPixelRect output_rect
);

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
    // Post-demosaic, scene-linear creative white balance in the declared D65 working space.
    // Temperature and tint are normalized user intent in [-1, 1]. Positive
    // temperature warms the image; positive tint moves away from green toward
    // magenta. This runs before the tone controls in Shadow's default recipe, but is still
    // deliberately distinct from sensor-domain RAW WB.
    double temperature = 0.0;
    double tint = 0.0;
};

struct SaturationAdjustment final {
    // Perceptual Oklab/Oklch chroma scaling in the declared D65 working space. factor=0 is
    // monochrome at the same Oklab lightness; factor=1 is an exact no-op. This deliberately
    // leaves scene-linear extended-gamut values unclamped so later output rendering, rather
    // than a creative control, owns gamut mapping.
    double factor = 1.0;
};

// Scene-referred regional tone controls expressed as bounded, implementation-independent
// amounts. Their implementation deliberately distinguishes the endpoints (Blacks/Whites)
// from the broad recovery ranges (Shadows/Highlights): endpoint controls shape a tighter
// toe/shoulder response while recovery controls apply a wider EV-domain exposure field.
//
// The v1 contract evaluates those fields against a complete self-guided filter in log scene
// luminance, then applies the resulting EV gain to Oklab lightness while preserving a/b. The
// filter averages its local linear coefficients in a second box pass, so neighbouring pixels in
// the same tonal region share a gain (preserving local contrast) while high-contrast edges remain
// boundaries for the mask. Zeroes are exactly neutral and do not allocate any spatial working
// state.
struct SelectiveToneAdjustment final {
    double highlights = 0.0;
    double shadows = 0.0;
    double whites = 0.0;
    double blacks = 0.0;
};

inline constexpr std::uint32_t selective_tone_parameter_schema_version = 1;
inline constexpr std::uint32_t selective_tone_implementation_version = 1;

// Native/full-resolution radius of each box pass in the deterministic self-guided log-luminance
// filter. The executor converts this independently for each raster axis, so a warm proxy and a
// detail tile describe the same physical neighbourhood. The complete guided filter consumes two
// such passes, and therefore declares twice this radius to the tile scheduler.
inline constexpr double selective_tone_guided_mask_radius_level_zero = 48.0;
inline constexpr std::uint32_t selective_tone_guided_filter_box_passes = 2U;

inline constexpr std::size_t perceptual_hue_band_count = 8U;
inline constexpr std::size_t maximum_point_color_ranges = 16U;
inline constexpr std::size_t selective_color_target_count = 9U;
inline constexpr std::size_t selective_color_component_count = 4U;
inline constexpr std::size_t selective_color_value_count =
    selective_color_target_count * selective_color_component_count;
inline constexpr std::uint32_t perceptual_color_parameter_schema_version = 1;
inline constexpr std::uint32_t perceptual_color_implementation_version = 1;
inline constexpr std::size_t oklab_color_warper_grid_side = 5U;
inline constexpr std::size_t oklab_color_warper_control_point_count =
    oklab_color_warper_grid_side * oklab_color_warper_grid_side;
inline constexpr double oklab_color_warper_half_extent = 0.32;
inline constexpr double oklab_color_warper_maximum_offset = 0.32;
inline constexpr std::uint32_t oklab_color_warper_parameter_schema_version = 1;
inline constexpr std::uint32_t oklab_color_warper_implementation_version = 1;

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
// yellow, green, aqua, blue, purple, magenta. The v1 implementation anchors those names at
// the non-uniform Oklch hues of representative linear-sRGB colors and uses a smooth periodic
// partition of unity between adjacent anchors; it must never be interpreted as an HSV wheel.
// hue values in [-1, 1] map to [-30, 30] degrees; saturation/lightness and vibrance are
// normalized amounts in [-1, 1]. Achromatic pixels are deliberately left unchanged because
// hue is undefined at low chroma.
struct PerceptualColorAdjustment final {
    // Broad Oklab opponent-axis balancing, applied after the hue-keyed
    // controls. Positive a moves toward red (negative toward green); positive
    // b moves toward yellow (negative toward blue). These are deliberately
    // bounded user intents rather than an alternate camera white balance: RAW
    // input rendering and white balance stay earlier in the pipeline.
    double global_a_balance = 0.0;
    double global_b_balance = 0.0;
    double vibrance = 0.0;
    std::array<double, perceptual_hue_band_count> hue{};
    std::array<double, perceptual_hue_band_count> saturation{};
    std::array<double, perceptual_hue_band_count> lightness{};
    PerceptualColorRange color_range;
    std::vector<PerceptualColorRange> additional_color_ranges;
    // Photoshop-style Selective Color. Target order is red, yellow, green,
    // cyan, blue, magenta, white, neutral, black; component order is CMYK.
    // Relative is the default (changes existing ink proportionally), while
    // Absolute adds the requested ink directly.
    bool selective_color_relative = true;
    // Blend the resulting Oklab L back toward the source lightness after the
    // familiar CMYK correction. Zero preserves the Photoshop-like behaviour;
    // one makes Selective Color a hue/chroma-only correction in the perceptual
    // layer. The target masks are always evaluated in Oklab/OKLCH.
    double selective_color_lightness_protection = 0.0;
    std::array<std::array<double, selective_color_component_count>,
               selective_color_target_count> selective_color_cmyk{};
};

// A Color Warper is a fixed 5×5 lattice over the Oklab a/b plane. Points are
// stored as target displacements, row-major from negative to positive b and
// then negative to positive a. The source lattice is immutable, so a recipe
// remains compact and a UI can draw a stable mesh without serializing a second
// copy of every source coordinate. Bilinear interpolation makes adjacent point
// moves continuous; colors outside the declared Oklab extent fade to no warp.
//
// It is intentionally a separate node operation rather than another Point
// Color range. Point Color selects one sampled circular hue range. Color
// Warper moves a connected two-dimensional hue/chroma field, which makes it
// useful for coordinated palette reshaping and for a masked grade node.
struct OklabColorWarperControlPoint final {
    double a_offset = 0.0;
    double b_offset = 0.0;
};

struct OklabColorWarperAdjustment final {
    std::array<OklabColorWarperControlPoint, oklab_color_warper_control_point_count>
        control_points{};
    // Strength intentionally scales a complete authored lattice. It allows a
    // stable recipe to be blended without changing its geometry.
    double strength = 1.0;
};

// Immutable 3D `.cube` resource applied in processed working RGB. Intensity
// linearly blends the sampled result with the node input; zero is an exact
// no-op and permits an empty LUT for a stable, unselected Recipe slot.
struct CubeLutAdjustment final {
    CubeLut3D lut;
    double intensity = 0.0;
};

// The visible Detail & Effects control bundle is deliberately kept intact at
// the UI/CXX boundary. The Recipe assigns one of these internal passes to each
// copy of the bundle, preventing a creative LUT from accidentally
// moving technical recovery or grain/vignette work across the pipeline.
enum class DetailEffectsExecutionPass : std::uint8_t {
    technical_detail,
    color_grading,
    finishing_effects,
};

// Luminance-only capture sharpening in scene-linear RGB. radius is the level-0 Gaussian sigma;
// threshold maps linearly to at most 0.25 EV of soft-thresholding. A common gain is applied to
// R, G, and B so sharpening cannot introduce chromatic fringes by treating channels separately.
struct SharpenAdjustment final {
    DetailEffectsExecutionPass execution_pass = DetailEffectsExecutionPass::technical_detail;
    double amount = 0.0;
    double radius = 1.0;
    double threshold = 0.0;
    double masking = 0.0;
    // Multi-scale perceptual detail in Oklab L. Clarity acts on protected
    // mid-frequency structure; Texture acts on the smaller residual. Both
    // are signed, leave Oklab a/b intact, and therefore cannot directly
    // rotate hue or change chroma.
    double clarity = 0.0;
    double texture = 0.0;
    // Edge-aware broad local-contrast band in Oklab L. `local_contrast_scale`
    // blends its native support from a medium to a large photographic radius;
    // it remains separate from clarity (mid-frequency) and texture (fine
    // residual) so the three controls retain distinct spatial meanings.
    double local_contrast = 0.0;
    double local_contrast_scale = 0.5;
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

// The 37-scalar Detail & Effects wire shape is divided into three ordered
// execution passes. During pre-release development these all remain v1; old
// local Recipes are discarded when the shape or behavior changes.
inline constexpr std::uint32_t detail_effects_parameter_schema_version = 1;
inline constexpr std::uint32_t technical_detail_implementation_version = 1;
inline constexpr std::uint32_t color_grading_implementation_version = 1;
inline constexpr std::uint32_t finishing_effects_implementation_version = 1;

inline constexpr std::uint32_t oklab_lightness_tone_curve_parameter_schema_version = 1;
inline constexpr std::uint32_t oklab_lightness_tone_curve_implementation_version = 1;
inline constexpr std::uint32_t oklab_opponent_tone_curve_parameter_schema_version = 1;
inline constexpr std::uint32_t oklab_opponent_tone_curve_implementation_version = 1;
// The perceptual L curve has at most this many authored knots.
inline constexpr std::size_t maximum_tone_curve_points = 256U;
inline constexpr std::size_t maximum_tone_curve_preview_samples = 4'097U;

struct ToneCurvePoint final {
    double x = 0.0;
    double y = 0.0;

    auto operator<=>(const ToneCurvePoint&) const = default;
};

// One set of Oklab-L interpolation knots. The implementation fits a local,
// shape-preserving Fritsch-Butland PCHIP through these points. Strictly increasing
// x coordinates span [0, 1]; y remains unbounded.
struct ToneCurveSet final {
    std::vector<ToneCurvePoint> points{{0.0, 0.0}, {1.0, 1.0}};
};

// The sole user-authored tone curve is evaluated only on Oklab L. It deliberately
// keeps the opponent a/b axes unchanged, so tonal shaping preserves hue and chroma
// much more faithfully than an RGB curve. The normalized control domain and linear
// endpoint extrapolation retain scene-linear HDR headroom.
struct OklabLightnessToneCurve final {
    std::uint32_t parameter_schema_version = oklab_lightness_tone_curve_parameter_schema_version;
    std::uint32_t implementation_version = oklab_lightness_tone_curve_implementation_version;
    ToneCurveSet lightness;
};

// Two optional, lightness-keyed Oklab opponent offsets. Unlike RGB channel
// curves, these remain in a perceptual color space: the `a` curve moves from
// green to red and the `b` curve moves from blue to yellow at each lightness.
// They are deliberately a separate advanced operation, so a normal tone curve
// never starts changing hue merely because it shares curve-editor affordances.
struct OklabOpponentToneCurves final {
    std::uint32_t parameter_schema_version = oklab_opponent_tone_curve_parameter_schema_version;
    std::uint32_t implementation_version = oklab_opponent_tone_curve_implementation_version;
    ToneCurveSet a{.points = {{0.0, 0.0}, {1.0, 0.0}}};
    ToneCurveSet b{.points = {{0.0, 0.0}, {1.0, 0.0}}};
};

// One small non-generative repair. Coordinates are normalized to the full
// original-oriented image; the radius remains in level-zero pixels so warm
// proxies and full detail apply the same physical selection.
enum class SpotRepairMode : std::uint8_t {
    heal = 0U,
    clone = 1U,
};

struct SpotHealTarget final {
    double center_x = 0.5;
    double center_y = 0.5;
    std::uint16_t radius_level_zero_pixels = 1U;
    SpotRepairMode mode = SpotRepairMode::heal;
    double source_offset_x_radii = 0.0;
    double source_offset_y_radii = 0.0;
    double feather = 0.28;
};

// A single authored point on a continuous Repair/Clone brush stroke. Coordinates
// use the same full-image normalized space as legacy spots and local masks, so a
// stroke remains aligned between warm previews and full-detail tiles.
struct RetouchStrokePoint final {
    double x = 0.5;
    double y = 0.5;
};

// One non-destructive continuous retouch gesture. The renderer sweeps the
// level-zero radius along adjacent points as a union of round-ended capsules;
// it is deliberately one adjustment unit rather than a persisted row of spots.
// Clone offsets remain measured in brush radii and are fixed for the complete
// stroke, keeping the source region aligned to the target path.
struct RetouchStroke final {
    std::vector<RetouchStrokePoint> points;
    std::uint16_t radius_level_zero_pixels = 1U;
    SpotRepairMode mode = SpotRepairMode::heal;
    double source_offset_x_radii = 0.0;
    double source_offset_y_radii = 0.0;
    double feather = 0.28;
};

struct SpotHealAdjustment final {
    // Legacy single-click repair targets remain first-class so old recipes keep
    // their exact behavior. New drags are represented by one RetouchStroke.
    std::vector<SpotHealTarget> spots;
    std::vector<RetouchStroke> strokes;
};

using AdjustmentParameters = std::variant<
    ExposureAdjustment,
    ContrastAdjustment,
    OklabLightnessToneCurve,
    OklabOpponentToneCurves,
    RgbWhiteBalanceAdjustment,
    SaturationAdjustment,
    SelectiveToneAdjustment,
    PerceptualColorAdjustment,
    OklabColorWarperAdjustment,
    CubeLutAdjustment,
    SharpenAdjustment,
    SpotHealAdjustment>;

enum class AdjustmentOperation : std::uint8_t {
    exposure,
    contrast,
    oklab_lightness_tone_curve,
    oklab_opponent_tone_curves,
    rgb_white_balance,
    saturation,
    selective_tone,
    perceptual_color,
    oklab_color_warper,
    lut_3d,
    sharpen,
    spot_heal,
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
// Backend-neutral execution-plan identity. CPU, Metal, and future backends may consume the
// same compiled ordering contract; a semantic change to segmentation, neutral-node elision,
// or footprint accumulation must publish a new version and canonical identity.
inline constexpr std::uint32_t edit_execution_plan_identity_version = 1;
inline constexpr std::string_view edit_execution_plan_identity =
    "shadow.edit-execution-plan.v1";
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

enum class EditErrorCode : std::uint8_t {
    invalid_image_layout,
    incompatible_color_encoding,
    invalid_working_space,
    invalid_parameter,
    unsupported_version,
    non_finite_value,
    numeric_overflow,
    backend_failure,
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
// Pass-aware locality for scheduling. The legacy operation-only overload is
// intentionally conservative for callers that do not retain parameters.
[[nodiscard]] AdjustmentLocality locality(const AdjustmentParameters& parameters) noexcept;
[[nodiscard]] AdjustmentLocality locality(AdjustmentOperation operation) noexcept;
// The supplied scales are level-0-to-raster sampling densities. Invalid/non-finite scales or
// malformed parameters fail closed; callers normally validate the full node plan first.
[[nodiscard]] AdjustmentFootprint footprint(
    const AdjustmentParameters& parameters,
    double level_zero_to_raster_scale_x = 1.0,
    double level_zero_to_raster_scale_y = 1.0
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
};

// Validates and deterministically repairs small spots from a smooth ring of
// surrounding pixels. This deliberately is not an inpainting/generative API:
// its result is fully determined by the current raster and stored targets.
void validate_spot_heal(const SpotHealAdjustment& adjustment);
void apply_spot_heal(
    FloatRgbImage& image,
    const SpotHealAdjustment& adjustment,
    AdjustmentExecutionContext context = {}
);

// A normalized selection shape owned by one adjustment-layer instance. It is
// intentionally independent from AdjustmentNode: a complete Grade Node still
// owns all of its color/tone controls, while this value only describes where
// its before/after blend is visible.
enum class LocalMaskKind : std::uint8_t {
    linear_gradient,
    radial_gradient,
    brush,
};

struct LocalMaskPoint final {
    double x = 0.0;
    double y = 0.0;
    bool begins_stroke = false;
};

struct LocalMask final {
    LocalMaskKind kind = LocalMaskKind::linear_gradient;
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 1.0;
    double y1 = 0.0;
    double radius_x = 0.0;
    double radius_y = 0.0;
    double feather = 0.0;
    bool invert = false;
    std::vector<LocalMaskPoint> points;
};

// A sequential Grade Node layer. The first implementation supports only
// Normal blending; opacity and an optional spatial mask define the mix between
// the image before and after the enclosed adjustment chain.
struct AdjustmentLayer final {
    std::string layer_id;
    bool enabled = true;
    double opacity = 1.0;
    std::optional<LocalMask> mask;
    std::vector<AdjustmentNode> nodes;
};

// Executes an intentionally compact subset of the future typed edit graph. The recommended
// default pipeline order is RgbWhiteBalance -> Exposure -> Contrast -> SelectiveTone ->
// Saturation -> PerceptualColor -> OklabLightnessToneCurve -> OklabOpponentToneCurves, but that is a recipe
// convention: this executor always applies nodes in the supplied span order.
// Disabled nodes are skipped and the input is never mutated. The executor does not clamp
// negative or >1 values and rejects NaN/Inf rather than silently contaminating caches.
[[nodiscard]] FloatRgbImage execute_adjustment_nodes(
    const FloatRgbImage& input,
    std::span<const AdjustmentNode> nodes,
    AdjustmentExecutionContext context = {}
);

// Executes complete Grade Node layers. Unmasked, fully opaque layers take the
// same direct node path; masked layers render a temporary result then blend it
// with the incoming image in original normalized coordinates. This preserves
// exact placement between warm proxies and independently requested detail
// tiles.
[[nodiscard]] FloatRgbImage execute_adjustment_layers(
    const FloatRgbImage& input,
    std::span<const AdjustmentLayer> layers,
    AdjustmentExecutionContext context = {}
);

// Applies a smooth curve to Oklab L only.  This standalone equivalent of an
// OklabLightnessToneCurve node is useful both for contract tests and future
// GPU parity tests; no output gamut clipping occurs here.
[[nodiscard]] FloatRgbImage apply_oklab_lightness_tone_curve(
    const FloatRgbImage& input,
    const OklabLightnessToneCurve& curve
);

// Samples the exact perceptual-curve evaluator at uniformly spaced x coordinates in [0, 1].
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
// Cache provenance for one completed warm-preview render. Adjustment and same-size display are
// independent provenance-bearing CPU/Metal stages. This belongs to the render result rather than
// the immutable session or generic EncodedProxy payload.
inline constexpr std::uint32_t edit_preview_execution_receipt_schema_version = 1U;
inline constexpr std::uint32_t edit_preview_cpu_adjustment_backend_version = 1U;
inline constexpr std::uint32_t edit_preview_metal_adjustment_backend_version = 1U;
inline constexpr std::uint32_t edit_preview_cpu_display_backend_version = 1U;
inline constexpr std::uint32_t edit_preview_metal_display_backend_version = 1U;
inline constexpr std::uint32_t edit_preview_jpeg_444_contract_version = 1U;

enum class EditPreviewBackend : std::uint8_t {
    cpu,
    metal,
};

struct EditPreviewExecutionReceipt final {
    std::uint32_t schema_version = edit_preview_execution_receipt_schema_version;
    EditPreviewBackend adjustment_backend = EditPreviewBackend::cpu;
    std::uint32_t adjustment_backend_version =
        edit_preview_cpu_adjustment_backend_version;
    std::uint32_t adjustment_execution_contract_version =
        edit_execution_plan_identity_version;
    EditPreviewBackend display_backend = EditPreviewBackend::cpu;
    std::uint32_t display_backend_version = edit_preview_cpu_display_backend_version;
    std::uint32_t display_output_contract_version =
        display_srgb8_output_transform_version;
    // The session-resident Metal route is structurally distinct from the
    // staged adjustment/display route. Keep that fact explicit instead of
    // encoding a route choice by inflating a backend version number.
    bool fused_pipeline = false;
    // These fields are diagnostic only. If automatic acceleration falls back, the complete
    // affected stage must restart from its immutable input; the effective CPU/Metal route above
    // then completely identifies the output math.
    bool adjustment_fell_back = false;
    bool display_fell_back = false;
    std::string diagnostic;

    [[nodiscard]] bool valid() const noexcept;
};

// Canonical cache-safe identity. It contains only fixed backend/contract identifiers; fallback
// diagnostics, local device information and user-local paths are deliberately excluded.
[[nodiscard]] std::string edit_preview_execution_receipt_identity(
    const EditPreviewExecutionReceipt& receipt
);

// Build/runtime-independent implementation contract known before a source is decoded. The
// desktop combines this with its bounded source-environment identity to reject stale gallery
// previews, while the per-render receipt above distinguishes the effective CPU/Metal route.
[[nodiscard]] std::string edit_preview_generator_implementation_identity();

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
    EditPreviewExecutionReceipt execution;
};

struct WarmEditPreviewGpuStats final {
    bool resident = false;
    std::uint64_t source_upload_count = 0U;
    std::uint64_t gpu_buffer_allocation_count = 0U;
    std::uint64_t render_count = 0U;
    std::uint64_t completed_render_count = 0U;
    std::uint64_t peak_concurrent_renders = 0U;
    std::uint64_t curve_resource_upload_count = 0U;
    std::uint64_t lut_resource_upload_count = 0U;
    std::uint64_t perceptual_mixer_resource_upload_count = 0U;
    std::uint64_t perceptual_range_resource_upload_count = 0U;
    std::uint64_t selective_color_resource_upload_count = 0U;
    std::uint64_t resource_cache_hit_count = 0U;
    std::uint64_t resident_bytes = 0U;

    auto operator<=>(const WarmEditPreviewGpuStats&) const = default;
};

template <typename T>
struct CancellableEditPreviewResult final {
    std::optional<T> completed;

    [[nodiscard]] bool cancelled() const noexcept {
        return !completed.has_value();
    }
};

namespace detail {
class WarmEditGpuSession;
}

class WarmEditPreviewSession final {
public:
    WarmEditPreviewSession(const WarmEditPreviewSession&) = delete;
    WarmEditPreviewSession& operator=(const WarmEditPreviewSession&) = delete;
    WarmEditPreviewSession(WarmEditPreviewSession&&) noexcept = default;
    WarmEditPreviewSession& operator=(WarmEditPreviewSession&&) noexcept = default;
    ~WarmEditPreviewSession() = default;

    [[nodiscard]] Dimensions dimensions() const noexcept;
    [[nodiscard]] std::uint32_t max_edge() const noexcept;
    // Provenance of the provider render retained by this preview. It remains separate from the
    // editable recipe and from the later optical-correction receipt.
    [[nodiscard]] const RawDevelopmentReceipt& raw_development_receipt() const noexcept;
    [[nodiscard]] const RawPipelineReceipt& raw_pipeline_receipt() const noexcept;
    [[nodiscard]] const OpticsProfileReceipt& optics_receipt() const noexcept;
    // Runtime-only observability for tests and future diagnostics. These counters never enter
    // Recipe, catalog, or cache identities.
    [[nodiscard]] WarmEditPreviewGpuStats gpu_stats() const noexcept;
    [[nodiscard]] EncodedProxy render_jpeg(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] EncodedProxy render_jpeg_layers(
        std::span<const AdjustmentLayer> layers,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] AnalyzedEditPreview render_jpeg_with_analysis(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] AnalyzedEditPreview render_jpeg_with_analysis_layers(
        std::span<const AdjustmentLayer> layers,
        std::uint8_t jpeg_quality = 95,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<EncodedProxy> render_jpeg_cancellable(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] CancellableEditPreviewResult<AnalyzedEditPreview>
    render_jpeg_with_analysis_cancellable(
        std::span<const AdjustmentNode> nodes,
        std::uint8_t jpeg_quality,
        std::stop_token cancellation,
        const PhotoGeometry& geometry = {}
    ) const;

private:
    WarmEditPreviewSession(
        FloatRgbImage working_proxy,
        std::uint32_t max_edge,
        RawDevelopmentReceipt raw_development_receipt,
        RawPipelineReceipt raw_pipeline_receipt,
        OpticsProfileReceipt optics_receipt
    );

    FloatRgbImage working_proxy_;
    std::uint32_t max_edge_ = 0;
    RawDevelopmentReceipt raw_development_receipt_;
    RawPipelineReceipt raw_pipeline_receipt_;
    OpticsProfileReceipt optics_receipt_;
    std::shared_ptr<detail::WarmEditGpuSession> warm_gpu_session_;
    std::string warm_gpu_diagnostic_;

    friend WarmEditPreviewSession prepare_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge,
        const OpticsProvider* optics_provider,
        const OpticsSettings& optics_settings
    );
    friend WarmEditPreviewSession prepare_warm_edit_preview(
        const DecodeSession& session,
        std::uint32_t max_edge,
        const RawDevelopmentPlan& raw_development_plan,
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
    [[nodiscard]] const RawDevelopmentReceipt& raw_development_receipt() const noexcept;
    [[nodiscard]] const RawPipelineReceipt& raw_pipeline_receipt() const noexcept;
    [[nodiscard]] const OpticsProfileReceipt& optics_receipt() const noexcept;
    [[nodiscard]] RenderedDetailTile render_rgb8(
        std::span<const AdjustmentNode> nodes,
        DetailTileRect rect,
        const PhotoGeometry& geometry = {}
    ) const;
    [[nodiscard]] RenderedDetailTile render_rgb8_layers(
        std::span<const AdjustmentLayer> layers,
        DetailTileRect rect,
        const PhotoGeometry& geometry = {}
    ) const;

private:
    FullEditDetailSession(
        PixelBuffer reference_rgb,
        std::uint64_t retained_bytes,
        RawDevelopmentReceipt raw_development_receipt,
        RawPipelineReceipt raw_pipeline_receipt,
        OpticsProfileReceipt optics_receipt,
        SourceRenderingReceipt source_rendering
    );

    PixelBuffer reference_rgb_;
    std::uint64_t retained_bytes_ = 0;
    // Kept separately from the post-optics raster: an independently implemented OpticsProvider
    // is allowed to allocate a new PixelBuffer and must not be able to erase decoder provenance.
    RawDevelopmentReceipt raw_development_receipt_;
    RawPipelineReceipt raw_pipeline_receipt_;
    OpticsProfileReceipt optics_receipt_;
    // Source rendering is independent from the editable Recipe. Retain its compact receipt so
    // full-resolution tiles apply the exact same standard/profile exposure as the warm proxy.
    SourceRenderingReceipt source_rendering_;

    friend FullEditDetailSession prepare_full_edit_detail(
        const DecodeSession& session,
        const OpticsProvider* optics_provider,
        const OpticsSettings& optics_settings
    );
    friend FullEditDetailSession prepare_full_edit_detail(
        const DecodeSession& session,
        const RawDevelopmentPlan& raw_development_plan,
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

// Same bounded preview contract, but with an explicit source-development request. The plan is
// not a user-visible adjustment node: it controls how a RAW provider produces the immutable
// source raster before the common RGB edit graph. JPEG/HEIF providers intentionally keep their
// legacy path because their pixels have already been developed.
[[nodiscard]] WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
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

// Requests the immutable source at full-resolution detail or export intent. A RAW provider may
// negotiate quality or policy details, but it must retain the requested/effective plan pair in
// the returned RawDevelopmentReceipt. Preview intent is rejected so a low-cost warm raster can
// never accidentally populate a 1:1 or export cache entry.
[[nodiscard]] FullEditDetailSession prepare_full_edit_detail(
    const DecodeSession& session,
    const RawDevelopmentPlan& raw_development_plan,
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

// Layer-aware equivalent of the standard reference proxy route. It is deliberately a separate
// entry point so the established flat-node call path keeps its accelerated implementation until
// a layer-aware GPU executor is available. The output contract remains the same JPEG proxy.
[[nodiscard]] EncodedProxy render_edited_reference_proxy_jpeg_layers(
    const DecodeSession& session,
    std::span<const AdjustmentLayer> layers,
    ProxyRequest request = {},
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

// Explicit plan-bearing forms used by cache-aware callers. Existing overloads above select the
// canonical preview plan, preserving their source-compatible behavior.
[[nodiscard]] EncodedProxy render_reference_proxy_jpeg(
    const DecodeSession& session,
    ProxyRequest request,
    const RawDevelopmentPlan& raw_development_plan
);

[[nodiscard]] EncodedProxy render_edited_reference_proxy_jpeg(
    const DecodeSession& session,
    std::span<const AdjustmentNode> nodes,
    ProxyRequest request,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

[[nodiscard]] EncodedProxy render_edited_reference_proxy_jpeg_layers(
    const DecodeSession& session,
    std::span<const AdjustmentLayer> layers,
    ProxyRequest request,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider = nullptr,
    const OpticsSettings& optics_settings = default_optics_settings()
);

} // namespace shadow::image
