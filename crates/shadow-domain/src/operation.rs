//! Stable identifiers shared by persisted Recipes and operation executors.
//!
//! Changing one of these strings changes the persisted operation contract. A
//! different parameter shape or implementation therefore needs a new schema
//! or implementation version instead of silently reusing an existing value.

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

pub const TONE_CURVE_OPERATION_ID: &str = "shadow.tone_curve";
pub const TONE_CURVE_POINTS_PARAMETER_KEY: &str = "points";
/// Parameter schema for the smooth master-plus-RGB Tone Curve contract.
///
/// Schema 1 remains the historical piecewise-linear `points` contract and
/// must never be reinterpreted by this implementation.
pub const TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION: u32 = 2;
/// Persisted executor implementation for the smooth RGB Tone Curve contract.
pub const TONE_CURVE_V2_IMPLEMENTATION_VERSION: &str = "cpu-reference-v2";
pub const TONE_CURVE_MASTER_POINTS_PARAMETER_KEY: &str = "master_points";
pub const TONE_CURVE_RED_POINTS_PARAMETER_KEY: &str = "red_points";
pub const TONE_CURVE_GREEN_POINTS_PARAMETER_KEY: &str = "green_points";
pub const TONE_CURVE_BLUE_POINTS_PARAMETER_KEY: &str = "blue_points";

pub const RGB_WHITE_BALANCE_OPERATION_ID: &str = "shadow.rgb_white_balance";
pub const WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY: &str = "temperature";
pub const WHITE_BALANCE_TINT_PARAMETER_KEY: &str = "tint";

pub const SATURATION_OPERATION_ID: &str = "shadow.saturation";
pub const SATURATION_FACTOR_PARAMETER_KEY: &str = "factor";

pub const SELECTIVE_TONE_OPERATION_ID: &str = "shadow.selective_tone";
/// The current Selective Tone evaluator uses an edge-aware guided log-luminance mask. It keeps
/// the four existing slider values but is intentionally a new persisted contract rather than a
/// reinterpretation of the old pixel-local evaluator.
pub const SELECTIVE_TONE_V2_PARAMETER_SCHEMA_VERSION: u32 = 2;
pub const SELECTIVE_TONE_V2_IMPLEMENTATION_VERSION: &str = "shadow-cpu-selective-tone-guided-v2";
pub const HIGHLIGHTS_PARAMETER_KEY: &str = "highlights";
pub const SHADOWS_PARAMETER_KEY: &str = "shadows";
pub const WHITES_PARAMETER_KEY: &str = "whites";
pub const BLACKS_PARAMETER_KEY: &str = "blacks";

pub const PERCEPTUAL_COLOR_OPERATION_ID: &str = "shadow.perceptual_color";
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
pub const PERCEPTUAL_COLOR_V2_IMPLEMENTATION_VERSION: &str = "shadow-cpu-perceptual-color-v2";

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
pub const DETAIL_EFFECTS_V2_IMPLEMENTATION_VERSION: &str = "shadow-cpu-detail-effects-v2";

/// Graph schema used by the current Basic adjustments layer.
pub const BASIC_GRAPH_SCHEMA_VERSION: u32 = 1;

/// Persisted label that identifies the current Basic adjustments layer.
pub const BASIC_LAYER_LABEL: &str = "Basic adjustments";

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{OperationId, ParameterKey};

    #[test]
    fn contract_identifiers_are_valid_domain_names() {
        for operation_id in [
            EXPOSURE_OPERATION_ID,
            CONTRAST_OPERATION_ID,
            TONE_CURVE_OPERATION_ID,
            RGB_WHITE_BALANCE_OPERATION_ID,
            SATURATION_OPERATION_ID,
            SELECTIVE_TONE_OPERATION_ID,
            PERCEPTUAL_COLOR_OPERATION_ID,
            LUT_3D_OPERATION_ID,
            SHARPEN_OPERATION_ID,
        ] {
            OperationId::new(operation_id).expect("operation contract id must remain valid");
        }

        for parameter_key in [
            EXPOSURE_STOPS_PARAMETER_KEY,
            CONTRAST_FACTOR_PARAMETER_KEY,
            CONTRAST_PIVOT_PARAMETER_KEY,
            TONE_CURVE_POINTS_PARAMETER_KEY,
            TONE_CURVE_MASTER_POINTS_PARAMETER_KEY,
            TONE_CURVE_RED_POINTS_PARAMETER_KEY,
            TONE_CURVE_GREEN_POINTS_PARAMETER_KEY,
            TONE_CURVE_BLUE_POINTS_PARAMETER_KEY,
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
}
