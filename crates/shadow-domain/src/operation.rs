//! Identifiers shared by persisted Recipes and operation executors.
//!
//! During pre-release development, contract changes replace the sole v1 shape
//! and invalidate old local data rather than accumulating compatibility
//! versions. Execution roles remain explicit types instead of version values.

/// Parameter schema used by the v1 CPU reference operations. A newer
/// per-operation contract must not silently upgrade unrelated nodes.
pub const CPU_REFERENCE_PARAMETER_SCHEMA_VERSION: u32 = 1;

/// Stable implementation identifier used by the v1 CPU reference executor.
pub const CPU_REFERENCE_IMPLEMENTATION_VERSION: &str = "cpu-reference-v1";
/// Numeric executor revision carried across the CXX render-plan boundary.
///
/// This remains separate from the persisted string so an old Recipe maps to
/// its original executor revision instead of silently adopting a future one.
pub const CPU_REFERENCE_IMPLEMENTATION_REVISION: u32 = 1;

pub const EXPOSURE_OPERATION_ID: &str = "shadow.exposure";
pub const EXPOSURE_STOPS_PARAMETER_KEY: &str = "stops";

pub const CONTRAST_OPERATION_ID: &str = "shadow.contrast";
pub const CONTRAST_FACTOR_PARAMETER_KEY: &str = "factor";
pub const CONTRAST_PIVOT_PARAMETER_KEY: &str = "pivot";

/// The sole authored point curve: perceptual Oklab lightness. It keeps hue and
/// chroma stable while changing brightness, rather than exposing RGB channels
/// whose output depends on the working-space primaries.
pub const OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID: &str = "shadow.oklab_lightness_tone_curve";
pub const OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY: &str = "lightness_points";
pub const OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION: &str =
    "shadow-cpu-oklab-lightness-tone-curve-v1";

pub const RGB_WHITE_BALANCE_OPERATION_ID: &str = "shadow.rgb_white_balance";
pub const WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY: &str = "temperature";
pub const WHITE_BALANCE_TINT_PARAMETER_KEY: &str = "tint";

pub const SATURATION_OPERATION_ID: &str = "shadow.saturation";
pub const SATURATION_FACTOR_PARAMETER_KEY: &str = "factor";

pub const SELECTIVE_TONE_OPERATION_ID: &str = "shadow.selective_tone";
/// The current Selective Tone evaluator uses a complete self-guided log-luminance filter. It
/// keeps the four slider values and averages the local a/b coefficients in a second box pass.
/// Until Shadow makes its first compatibility promise this implementation replaces earlier
/// development experiments in the sole v1 contract.
pub const SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const SELECTIVE_TONE_IMPLEMENTATION_VERSION: &str = "shadow-cpu-selective-tone-guided-v1";
pub const HIGHLIGHTS_PARAMETER_KEY: &str = "highlights";
pub const SHADOWS_PARAMETER_KEY: &str = "shadows";
pub const WHITES_PARAMETER_KEY: &str = "whites";
pub const BLACKS_PARAMETER_KEY: &str = "blacks";

pub const PERCEPTUAL_COLOR_OPERATION_ID: &str = "shadow.perceptual_color";
/// Broad Oklab opponent-axis balance. Positive `a` moves toward red and
/// positive `b` moves toward yellow; sensor-domain white balance remains an
/// earlier operation in the development plan.
pub const GLOBAL_A_BALANCE_PARAMETER_KEY: &str = "global_a_balance";
pub const GLOBAL_B_BALANCE_PARAMETER_KEY: &str = "global_b_balance";
pub const VIBRANCE_PARAMETER_KEY: &str = "vibrance";
pub const COLOR_MIXER_HUE_PARAMETER_KEY: &str = "mixer_hue";
pub const COLOR_MIXER_SATURATION_PARAMETER_KEY: &str = "mixer_saturation";
pub const COLOR_MIXER_LIGHTNESS_PARAMETER_KEY: &str = "mixer_lightness";
pub const COLOR_RANGE_ENABLED_PARAMETER_KEY: &str = "range_enabled";
pub const COLOR_RANGE_CENTER_PARAMETER_KEY: &str = "range_center";
pub const COLOR_RANGE_WIDTH_PARAMETER_KEY: &str = "range_width";
pub const COLOR_RANGE_SOFTNESS_PARAMETER_KEY: &str = "range_softness";
pub const COLOR_RANGE_HUE_PARAMETER_KEY: &str = "range_hue";
pub const COLOR_RANGE_SATURATION_PARAMETER_KEY: &str = "range_saturation";
pub const COLOR_RANGE_LIGHTNESS_PARAMETER_KEY: &str = "range_lightness";
pub const POINT_COLOR_RANGES_PARAMETER_KEY: &str = "point_color_ranges";
/// Photoshop-style Selective Color is intentionally part of the same complete
/// color-correction operation as the Color Mixer and Point Color controls.
/// The flattened value is nine target families × CMYK in the public order
/// red, yellow, green, cyan, blue, magenta, white, neutral, black.
pub const SELECTIVE_COLOR_RELATIVE_PARAMETER_KEY: &str = "selective_color_relative";
/// Blend the Oklab L result back toward the source after CMYK correction.
/// This keeps Selective Color's familiar authoring semantics while offering a
/// perceptual lightness lock for modern scene-referred grading.
pub const SELECTIVE_COLOR_LIGHTNESS_PROTECTION_PARAMETER_KEY: &str =
    "selective_color_lightness_protection";
pub const SELECTIVE_COLOR_CMYK_PARAMETER_KEY: &str = "selective_color_cmyk";
pub const PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION: &str = "shadow-cpu-perceptual-color-v1";

/// A fixed 5×5 Oklab a/b displacement lattice. This is intentionally a
/// separate operation from Color Mixer, Point Color, and Selective Color: it
/// moves a connected two-dimensional hue/chroma field.
pub const OKLAB_COLOR_WARPER_OPERATION_ID: &str = "shadow.oklab_color_warper";
/// Flattened row-major `(a_offset, b_offset)` pairs for the fixed 25 points.
pub const OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY: &str = "control_points";
/// Global blend of the authored lattice, where zero is a neutral bypass.
pub const OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY: &str = "strength";
pub const OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION: &str = "shadow-cpu-oklab-color-warper-v1";

pub const LUT_3D_OPERATION_ID: &str = "shadow.lut_3d";
pub const LUT_RESOURCE_ID_PARAMETER_KEY: &str = "resource_id";
pub const LUT_TITLE_PARAMETER_KEY: &str = "title";
pub const LUT_MANAGED_PATH_PARAMETER_KEY: &str = "managed_path";
pub const LUT_INTENSITY_PARAMETER_KEY: &str = "intensity";

pub const SHARPEN_OPERATION_ID: &str = "shadow.sharpen";
pub const SHARPEN_AMOUNT_PARAMETER_KEY: &str = "amount";
pub const SHARPEN_RADIUS_PARAMETER_KEY: &str = "radius";
pub const SHARPEN_THRESHOLD_PARAMETER_KEY: &str = "threshold";
pub const SHARPEN_MASKING_PARAMETER_KEY: &str = "masking";
pub const DETAIL_EFFECTS_PARAMETERS_KEY: &str = "detail_effects";

/// Technical pass over the complete Detail & Effects payload.
pub const TECHNICAL_DETAIL_OPERATION_ID: &str = "shadow.technical_detail";
pub const TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const TECHNICAL_DETAIL_IMPLEMENTATION_VERSION: &str = "shadow-cpu-technical-detail-v1";

/// Creative color-wheel pass kept out of the technical recovery stage.
pub const COLOR_GRADING_OPERATION_ID: &str = "shadow.color_grading";
pub const COLOR_GRADING_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const COLOR_GRADING_IMPLEMENTATION_VERSION: &str = "shadow-cpu-color-grading-v1";

/// Post-look pass evaluated after the LUT and color-grading chain.
pub const FINISHING_EFFECTS_OPERATION_ID: &str = "shadow.finishing_effects";
pub const FINISHING_EFFECTS_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const FINISHING_EFFECTS_IMPLEMENTATION_VERSION: &str = "shadow-cpu-finishing-effects-v1";

/// Graph schema used by the current default adjustment layer.
///
/// During pre-release development, changed graph shapes replace this v1
/// contract and old local Recipes are discarded rather than migrated.
pub const BASIC_GRAPH_SCHEMA_VERSION: u32 = 1;

/// Persisted label assigned to a newly created default adjustment layer.
///
/// It deliberately describes the node's role rather than limiting the controls it may grow to
/// contain. This leaves the name clear when AI-assisted tools become a separate surface.
pub const BASIC_LAYER_LABEL: &str = "Adjustments";

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{OperationId, ParameterKey};

    #[test]
    fn contract_identifiers_are_valid_domain_names() {
        for operation_id in [
            EXPOSURE_OPERATION_ID,
            CONTRAST_OPERATION_ID,
            OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID,
            RGB_WHITE_BALANCE_OPERATION_ID,
            SATURATION_OPERATION_ID,
            SELECTIVE_TONE_OPERATION_ID,
            PERCEPTUAL_COLOR_OPERATION_ID,
            OKLAB_COLOR_WARPER_OPERATION_ID,
            LUT_3D_OPERATION_ID,
            SHARPEN_OPERATION_ID,
            TECHNICAL_DETAIL_OPERATION_ID,
            COLOR_GRADING_OPERATION_ID,
            FINISHING_EFFECTS_OPERATION_ID,
        ] {
            OperationId::new(operation_id).expect("operation contract id must remain valid");
        }

        for parameter_key in [
            EXPOSURE_STOPS_PARAMETER_KEY,
            CONTRAST_FACTOR_PARAMETER_KEY,
            CONTRAST_PIVOT_PARAMETER_KEY,
            WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
            WHITE_BALANCE_TINT_PARAMETER_KEY,
            SATURATION_FACTOR_PARAMETER_KEY,
            HIGHLIGHTS_PARAMETER_KEY,
            SHADOWS_PARAMETER_KEY,
            WHITES_PARAMETER_KEY,
            BLACKS_PARAMETER_KEY,
            VIBRANCE_PARAMETER_KEY,
            COLOR_MIXER_HUE_PARAMETER_KEY,
            COLOR_MIXER_SATURATION_PARAMETER_KEY,
            COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
            COLOR_RANGE_ENABLED_PARAMETER_KEY,
            COLOR_RANGE_CENTER_PARAMETER_KEY,
            COLOR_RANGE_WIDTH_PARAMETER_KEY,
            COLOR_RANGE_SOFTNESS_PARAMETER_KEY,
            COLOR_RANGE_HUE_PARAMETER_KEY,
            COLOR_RANGE_SATURATION_PARAMETER_KEY,
            COLOR_RANGE_LIGHTNESS_PARAMETER_KEY,
            POINT_COLOR_RANGES_PARAMETER_KEY,
            SELECTIVE_COLOR_RELATIVE_PARAMETER_KEY,
            SELECTIVE_COLOR_LIGHTNESS_PROTECTION_PARAMETER_KEY,
            SELECTIVE_COLOR_CMYK_PARAMETER_KEY,
            OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY,
            OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY,
            LUT_RESOURCE_ID_PARAMETER_KEY,
            LUT_TITLE_PARAMETER_KEY,
            LUT_MANAGED_PATH_PARAMETER_KEY,
            LUT_INTENSITY_PARAMETER_KEY,
            SHARPEN_AMOUNT_PARAMETER_KEY,
            SHARPEN_RADIUS_PARAMETER_KEY,
            SHARPEN_THRESHOLD_PARAMETER_KEY,
            SHARPEN_MASKING_PARAMETER_KEY,
            DETAIL_EFFECTS_PARAMETERS_KEY,
        ] {
            ParameterKey::new(parameter_key)
                .expect("operation contract parameter key must remain valid");
        }
    }

    #[test]
    fn pre_release_operation_contracts_remain_v1() {
        assert_eq!(CPU_REFERENCE_PARAMETER_SCHEMA_VERSION, 1);
        assert_eq!(CPU_REFERENCE_IMPLEMENTATION_REVISION, 1);
        assert_eq!(OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION, 1);
        assert_eq!(SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION, 1);
        assert_eq!(OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION, 1);
        assert_eq!(TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION, 1);
        assert_eq!(COLOR_GRADING_PARAMETER_SCHEMA_VERSION, 1);
        assert_eq!(FINISHING_EFFECTS_PARAMETER_SCHEMA_VERSION, 1);
        assert_eq!(BASIC_GRAPH_SCHEMA_VERSION, 1);

        for identity in [
            CPU_REFERENCE_IMPLEMENTATION_VERSION,
            OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
            SELECTIVE_TONE_IMPLEMENTATION_VERSION,
            PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION,
            OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
            TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
            COLOR_GRADING_IMPLEMENTATION_VERSION,
            FINISHING_EFFECTS_IMPLEMENTATION_VERSION,
        ] {
            assert!(
                identity.ends_with("-v1"),
                "pre-release implementation identity escaped v1: {identity}"
            );
        }
    }
}
