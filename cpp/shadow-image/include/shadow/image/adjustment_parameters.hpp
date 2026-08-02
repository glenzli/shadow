#pragma once

#include <shadow/image/lut.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace shadow::image {

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
    double strength = 1.0;
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
    double strength = 1.0;
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

} // namespace shadow::image
