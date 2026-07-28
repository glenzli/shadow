//! Shared Detail & Effects payload, ordered pass roles, and parameter validation.

use crate::error::BridgeError;

use super::parameter_validation::validate_finite_render_parameter;

/// The visible Detail & Effects payload is one 37-value FFI record, compiled into three ordered
/// v1 internal passes.
pub const TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const TECHNICAL_DETAIL_IMPLEMENTATION_VERSION: u32 = 1;
pub const COLOR_GRADING_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const COLOR_GRADING_IMPLEMENTATION_VERSION: u32 = 1;
pub const FINISHING_EFFECTS_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const FINISHING_EFFECTS_IMPLEMENTATION_VERSION: u32 = 1;

/// Explicit processing role for the shared Detail & Effects parameter bundle.
///
/// This is intentionally independent from contract versions: all three roles use the current v1
/// contract, while this enum determines pipeline placement.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum AdjustmentDetailEffectsPass {
    TechnicalDetail,
    ColorGrading,
    FinishingEffects,
}

impl AdjustmentDetailEffectsPass {
    pub(super) const fn contract_versions(self) -> (u32, u32) {
        match self {
            Self::TechnicalDetail => (
                TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION,
                TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
            ),
            Self::ColorGrading => (
                COLOR_GRADING_PARAMETER_SCHEMA_VERSION,
                COLOR_GRADING_IMPLEMENTATION_VERSION,
            ),
            Self::FinishingEffects => (
                FINISHING_EFFECTS_PARAMETER_SCHEMA_VERSION,
                FINISHING_EFFECTS_IMPLEMENTATION_VERSION,
            ),
        }
    }
}

/// Spatial sharpening, detail, and finishing controls. `radius` is the level-0 Gaussian sigma in
/// pixels; the remaining values are normalized user intent.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SharpenParameters {
    pub amount: f64,
    pub radius: f64,
    pub threshold: f64,
    pub masking: f64,
    /// Signed Oklab-L multi-scale detail controls. Clarity operates on the edge-protected middle
    /// residual; Texture operates on the finer residual.
    pub clarity: f64,
    pub texture: f64,
    /// Broad edge-aware Oklab-L contrast, intentionally distinct from the smaller-frequency
    /// Clarity and Texture bands.
    pub local_contrast: f64,
    /// Normalized native support scale for `local_contrast`.
    pub local_contrast_scale: f64,
    pub denoise_luminance: f64,
    pub denoise_detail: f64,
    pub denoise_color: f64,
    pub dehaze: f64,
    pub defringe_purple_amount: f64,
    pub defringe_purple_hue_low: f64,
    pub defringe_purple_hue_high: f64,
    pub defringe_green_amount: f64,
    pub defringe_green_hue_low: f64,
    pub defringe_green_hue_high: f64,
    pub shadows_hue: f64,
    pub shadows_saturation: f64,
    pub shadows_luminance: f64,
    pub midtones_hue: f64,
    pub midtones_saturation: f64,
    pub midtones_luminance: f64,
    pub highlights_hue: f64,
    pub highlights_saturation: f64,
    pub highlights_luminance: f64,
    pub grading_blending: f64,
    pub grading_balance: f64,
    pub grain_amount: f64,
    pub grain_size: f64,
    pub grain_roughness: f64,
    pub vignette_amount: f64,
    pub vignette_midpoint: f64,
    pub vignette_roundness: f64,
    pub vignette_feather: f64,
    pub vignette_highlights: f64,
}

impl Default for SharpenParameters {
    fn default() -> Self {
        Self {
            amount: 0.0,
            radius: 1.0,
            threshold: 0.0,
            masking: 0.0,
            clarity: 0.0,
            texture: 0.0,
            local_contrast: 0.0,
            local_contrast_scale: 0.5,
            denoise_luminance: 0.0,
            denoise_detail: 0.5,
            denoise_color: 0.0,
            dehaze: 0.0,
            defringe_purple_amount: 0.0,
            defringe_purple_hue_low: 270.0,
            defringe_purple_hue_high: 340.0,
            defringe_green_amount: 0.0,
            defringe_green_hue_low: 100.0,
            defringe_green_hue_high: 165.0,
            shadows_hue: 0.0,
            shadows_saturation: 0.0,
            shadows_luminance: 0.0,
            midtones_hue: 0.0,
            midtones_saturation: 0.0,
            midtones_luminance: 0.0,
            highlights_hue: 0.0,
            highlights_saturation: 0.0,
            highlights_luminance: 0.0,
            grading_blending: 0.5,
            grading_balance: 0.0,
            grain_amount: 0.0,
            grain_size: 0.5,
            grain_roughness: 0.5,
            vignette_amount: 0.0,
            vignette_midpoint: 0.5,
            vignette_roundness: 0.0,
            vignette_feather: 0.5,
            vignette_highlights: 0.0,
        }
    }
}

fn validate_finite_detail_values(parameters: &SharpenParameters) -> Result<(), BridgeError> {
    for value in [
        parameters.amount,
        parameters.radius,
        parameters.threshold,
        parameters.masking,
        parameters.clarity,
        parameters.texture,
        parameters.local_contrast,
        parameters.local_contrast_scale,
        parameters.denoise_luminance,
        parameters.denoise_detail,
        parameters.denoise_color,
        parameters.defringe_purple_amount,
        parameters.defringe_green_amount,
        parameters.shadows_saturation,
        parameters.midtones_saturation,
        parameters.highlights_saturation,
        parameters.grading_blending,
        parameters.grain_amount,
        parameters.grain_size,
        parameters.grain_roughness,
        parameters.vignette_midpoint,
        parameters.vignette_feather,
        parameters.vignette_highlights,
    ] {
        validate_finite_render_parameter(value)?;
    }
    Ok(())
}

fn validate_signed_detail_values(parameters: &SharpenParameters) -> Result<(), BridgeError> {
    for value in [
        parameters.dehaze,
        parameters.clarity,
        parameters.texture,
        parameters.shadows_luminance,
        parameters.midtones_luminance,
        parameters.highlights_luminance,
        parameters.grading_balance,
        parameters.vignette_amount,
        parameters.vignette_roundness,
    ] {
        validate_finite_render_parameter(value)?;
        if !(-1.0..=1.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "signed Detail & Effects values must be in -1..=1",
            ));
        }
    }
    Ok(())
}

fn validate_color_grading_hues(parameters: &SharpenParameters) -> Result<(), BridgeError> {
    for value in [
        parameters.shadows_hue,
        parameters.midtones_hue,
        parameters.highlights_hue,
    ] {
        validate_finite_render_parameter(value)?;
        if !(0.0..=360.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "Color Grading hue values must be in 0..=360",
            ));
        }
    }
    Ok(())
}

fn validate_defringe_hues(parameters: &SharpenParameters) -> Result<(), BridgeError> {
    for value in [
        parameters.defringe_purple_hue_low,
        parameters.defringe_purple_hue_high,
        parameters.defringe_green_hue_low,
        parameters.defringe_green_hue_high,
    ] {
        validate_finite_render_parameter(value)?;
        if !(0.0..=360.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "defringe hue values must be in 0..=360",
            ));
        }
    }
    if parameters.defringe_purple_hue_low + 10.0 > parameters.defringe_purple_hue_high
        || parameters.defringe_green_hue_low + 10.0 > parameters.defringe_green_hue_high
    {
        return Err(BridgeError::InvalidEditRequest(
            "defringe hue ranges must have at least a 10 degree span",
        ));
    }
    Ok(())
}

fn bounded_detail_values_are_valid(parameters: &SharpenParameters) -> bool {
    (0.0..=2.0).contains(&parameters.amount)
        && (0.1..=5.0).contains(&parameters.radius)
        && (0.0..=1.0).contains(&parameters.threshold)
        && (0.0..=1.0).contains(&parameters.masking)
        && (-1.0..=1.0).contains(&parameters.clarity)
        && (-1.0..=1.0).contains(&parameters.texture)
        && (-1.0..=1.0).contains(&parameters.local_contrast)
        && (0.0..=1.0).contains(&parameters.local_contrast_scale)
        && [
            parameters.denoise_luminance,
            parameters.denoise_detail,
            parameters.denoise_color,
            parameters.defringe_purple_amount,
            parameters.defringe_green_amount,
            parameters.shadows_saturation,
            parameters.midtones_saturation,
            parameters.highlights_saturation,
            parameters.grading_blending,
            parameters.grain_amount,
            parameters.grain_size,
            parameters.grain_roughness,
            parameters.vignette_midpoint,
            parameters.vignette_feather,
            parameters.vignette_highlights,
        ]
        .into_iter()
        .all(|value| (0.0..=1.0).contains(&value))
}

pub(super) fn validate_sharpen(parameters: &SharpenParameters) -> Result<(), BridgeError> {
    validate_finite_detail_values(parameters)?;
    validate_signed_detail_values(parameters)?;
    validate_color_grading_hues(parameters)?;
    validate_defringe_hues(parameters)?;
    if !bounded_detail_values_are_valid(parameters) {
        return Err(BridgeError::InvalidEditRequest(
            "sharpen parameters are outside their contract",
        ));
    }
    Ok(())
}
