//! Perceptual Color, Color Mixer, Point Color, and Selective Color contracts.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

/// Fixed hue anchors used by the first perceptual Color Mixer contract.
pub const COLOR_MIXER_BAND_COUNT: usize = 8;
pub const MAX_POINT_COLOR_RANGES: usize = 16;
/// Photoshop-compatible Selective Color has six chromatic target families
/// plus white, neutral, and black. Each target owns CMYK amounts.
pub const SELECTIVE_COLOR_TARGET_COUNT: usize = 9;
pub const SELECTIVE_COLOR_COMPONENT_COUNT: usize = 4;
pub const SELECTIVE_COLOR_VALUE_COUNT: usize =
    SELECTIVE_COLOR_TARGET_COUNT * SELECTIVE_COLOR_COMPONENT_COUNT;
pub const PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION: u32 = 1;
/// One soft, circular hue range layered on top of the fixed Color Mixer.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ColorRangeParameters {
    pub enabled: bool,
    pub center_hue_degrees: f64,
    pub width_degrees: f64,
    pub softness: f64,
    pub hue_shift_degrees: f64,
    pub saturation: f64,
    pub lightness: f64,
}

impl Default for ColorRangeParameters {
    fn default() -> Self {
        Self {
            enabled: false,
            center_hue_degrees: 0.0,
            width_degrees: 30.0,
            softness: 0.5,
            hue_shift_degrees: 0.0,
            saturation: 0.0,
            lightness: 0.0,
        }
    }
}

/// Perceptual color controls evaluated together so Oklab conversion happens
/// once per pixel instead of once per UI slider.
#[derive(Debug, Clone, PartialEq)]
pub struct PerceptualColorParameters {
    /// Global green↔red Oklab opponent balance, independent of RAW white
    /// balance and intentionally applied in the perceptual color operation.
    pub global_a_balance: f64,
    /// Global blue↔yellow Oklab opponent balance.
    pub global_b_balance: f64,
    pub vibrance: f64,
    pub hue_shifts: [f64; COLOR_MIXER_BAND_COUNT],
    pub saturation: [f64; COLOR_MIXER_BAND_COUNT],
    pub lightness: [f64; COLOR_MIXER_BAND_COUNT],
    pub color_range: ColorRangeParameters,
    pub additional_color_ranges: Vec<ColorRangeParameters>,
    /// `true` mirrors Photoshop's default Relative method: corrections scale
    /// existing CMYK ink. `false` is the Absolute method.
    pub selective_color_relative: bool,
    /// 0 retains Photoshop-like CMYK lightness behaviour; 1 restores the
    /// source Oklab L after the correction so the tool becomes a hue/chroma
    /// correction with perceptual exposure protection.
    pub selective_color_lightness_protection: f64,
    /// Nine target families × cyan, magenta, yellow, black, all in [-1, 1].
    pub selective_color_cmyk: [f64; SELECTIVE_COLOR_VALUE_COUNT],
}

impl Default for PerceptualColorParameters {
    fn default() -> Self {
        Self {
            global_a_balance: 0.0,
            global_b_balance: 0.0,
            vibrance: 0.0,
            hue_shifts: [0.0; COLOR_MIXER_BAND_COUNT],
            saturation: [0.0; COLOR_MIXER_BAND_COUNT],
            lightness: [0.0; COLOR_MIXER_BAND_COUNT],
            color_range: ColorRangeParameters::default(),
            additional_color_ranges: Vec::new(),
            selective_color_relative: true,
            selective_color_lightness_protection: 0.0,
            selective_color_cmyk: [0.0; SELECTIVE_COLOR_VALUE_COUNT],
        }
    }
}

pub(super) fn validate_perceptual_color(
    parameters: &PerceptualColorParameters,
) -> Result<(), BridgeError> {
    for (value, name) in [
        (parameters.global_a_balance, "global Oklab a balance"),
        (parameters.global_b_balance, "global Oklab b balance"),
        (parameters.vibrance, "vibrance"),
    ] {
        validate_finite_render_parameter(value)?;
        if !(-1.0..=1.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(match name {
                "vibrance" => "vibrance must be in -1..=1",
                _ => "global Oklab balance must be in -1..=1",
            }));
        }
    }
    for values in [
        &parameters.hue_shifts,
        &parameters.saturation,
        &parameters.lightness,
    ] {
        for value in values {
            validate_finite_render_parameter(*value)?;
            if !(-1.0..=1.0).contains(value) {
                return Err(BridgeError::InvalidEditRequest(
                    "Color Mixer values must be in -1..=1",
                ));
            }
        }
    }
    if 1 + parameters.additional_color_ranges.len() > MAX_POINT_COLOR_RANGES {
        return Err(BridgeError::InvalidEditRequest(
            "Point Color supports at most 16 ordered ranges",
        ));
    }
    for range in
        std::iter::once(&parameters.color_range).chain(parameters.additional_color_ranges.iter())
    {
        for value in [
            range.center_hue_degrees,
            range.width_degrees,
            range.softness,
            range.hue_shift_degrees,
            range.saturation,
            range.lightness,
        ] {
            validate_finite_render_parameter(value)?;
        }
        if !(0.0..=360.0).contains(&range.center_hue_degrees)
            || !(1.0..=180.0).contains(&range.width_degrees)
            || !(0.0..=1.0).contains(&range.softness)
            || !(-180.0..=180.0).contains(&range.hue_shift_degrees)
            || !(-1.0..=1.0).contains(&range.saturation)
            || !(-1.0..=1.0).contains(&range.lightness)
        {
            return Err(BridgeError::InvalidEditRequest(
                "perceptual color range parameters are outside their contract",
            ));
        }
    }
    validate_finite_render_parameter(parameters.selective_color_lightness_protection)?;
    if !(0.0..=1.0).contains(&parameters.selective_color_lightness_protection) {
        return Err(BridgeError::InvalidEditRequest(
            "Selective Color lightness protection must be in 0..=1",
        ));
    }
    for value in &parameters.selective_color_cmyk {
        validate_finite_render_parameter(*value)?;
        if !(-1.0..=1.0).contains(value) {
            return Err(BridgeError::InvalidEditRequest(
                "Selective Color CMYK values must be in -1..=1",
            ));
        }
    }
    Ok(())
}
