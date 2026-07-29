//! Pixel predicate types, numeric domains, and deterministic local-detail semantics.

use serde::{Deserialize, Serialize};

use crate::recipe::{FiniteF64, RecipeValidationError, UnitInterval};

pub const MAX_LOCAL_DETAIL_RADIUS_LEVEL_ZERO_PIXELS: u16 = 64;

/// Oklch chroma is normalized as `clamp(C / 0.4, 0, 1)` for persisted
/// thresholds. The fixed divisor covers display-referred sRGB while leaving
/// headroom for the extended working values accepted by the edit pipeline.
pub const OKLCH_CHROMA_NORMALIZATION: f64 = 0.4;

/// The local-detail response is normalized as
/// `clamp(abs(L - box_mean(L)) / 0.25, 0, 1)`.
pub const LOCAL_DETAIL_RESIDUAL_NORMALIZATION: f64 = 0.25;

/// One deterministic condition evaluated against a Grade Node's input pixels.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum ConditionMaskPredicate {
    /// Oklab `L` in `[0, 1]`.
    OklabLightnessRange {
        lower: UnitInterval,
        upper: UnitInterval,
        softness: UnitInterval,
    },
    /// Circular Oklch hue plus an optional absolute normalized-chroma gate.
    ///
    /// `softness` is the existing hue-edge feather relative to half-width.
    /// `minimum_chroma_feather` is independent and expressed in normalized
    /// absolute-chroma units. A zero minimum disables the absolute gate, while
    /// the existing relative-chroma confidence still suppresses unstable hue
    /// near the neutral axis.
    OklchHueRange {
        center_hue_degrees: FiniteF64,
        half_width_degrees: FiniteF64,
        minimum_chroma: UnitInterval,
        #[serde(default = "zero_unit_interval")]
        minimum_chroma_feather: UnitInterval,
        softness: UnitInterval,
    },
    /// Oklch chroma normalized by [`OKLCH_CHROMA_NORMALIZATION`].
    OklchChromaRange {
        lower: UnitInterval,
        upper: UnitInterval,
        softness: UnitInterval,
    },
    /// A scale-explicit local-detail response. This is persistable but not
    /// executable until a renderer advertises the exact algorithm contract.
    LocalDetailRange {
        input: LocalDetailInput,
        algorithm: LocalDetailAlgorithm,
        radius_level_zero_pixels: u16,
        lower: UnitInterval,
        upper: UnitInterval,
        softness: UnitInterval,
    },
}

impl ConditionMaskPredicate {
    pub const fn oklab_lightness_range(
        lower: UnitInterval,
        upper: UnitInterval,
        softness: UnitInterval,
    ) -> Self {
        Self::OklabLightnessRange {
            lower,
            upper,
            softness,
        }
    }

    /// Creates a canonical circular Oklch-hue condition.
    ///
    /// # Errors
    ///
    /// Returns an error for non-finite values or a half-width outside
    /// `1°..=180°`.
    pub fn oklch_hue_range(
        center_hue_degrees: f64,
        half_width_degrees: f64,
        minimum_chroma: UnitInterval,
        softness: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        Self::oklch_hue_range_with_chroma_feather(
            center_hue_degrees,
            half_width_degrees,
            minimum_chroma,
            UnitInterval::ZERO,
            softness,
        )
    }

    /// Creates a canonical circular Oklch-hue condition with a separately
    /// authored absolute-chroma feather.
    ///
    /// # Errors
    ///
    /// Returns an error for non-finite values, a half-width outside
    /// `1°..=180°`, or a non-zero feather on a disabled zero-minimum gate.
    pub fn oklch_hue_range_with_chroma_feather(
        center_hue_degrees: f64,
        half_width_degrees: f64,
        minimum_chroma: UnitInterval,
        minimum_chroma_feather: UnitInterval,
        softness: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        let center = FiniteF64::new(center_hue_degrees)?;
        let center = center.get().rem_euclid(360.0);
        let center = if center == 0.0 { 0.0 } else { center };
        let predicate = Self::OklchHueRange {
            center_hue_degrees: FiniteF64::new(center)?,
            half_width_degrees: FiniteF64::new(half_width_degrees)?,
            minimum_chroma,
            minimum_chroma_feather,
            softness,
        };
        validate_predicate(&predicate)?;
        Ok(predicate)
    }

    pub const fn oklch_chroma_range(
        lower: UnitInterval,
        upper: UnitInterval,
        softness: UnitInterval,
    ) -> Self {
        Self::OklchChromaRange {
            lower,
            upper,
            softness,
        }
    }

    /// Creates a scale-explicit local-detail condition.
    ///
    /// # Errors
    ///
    /// Returns an error unless the level-zero radius is in `1..=64` and the
    /// normalized bounds are ordered.
    pub fn local_detail_range(
        radius_level_zero_pixels: u16,
        lower: UnitInterval,
        upper: UnitInterval,
        softness: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        let predicate = Self::LocalDetailRange {
            input: LocalDetailInput::LayerInputOklabLightness,
            algorithm: LocalDetailAlgorithm::BoxMeanAbsoluteResidualV1,
            radius_level_zero_pixels,
            lower,
            upper,
            softness,
        };
        validate_predicate(&predicate)?;
        Ok(predicate)
    }
}

/// The exact local-detail input channel. It is named in the payload so a
/// future RGB-luma or sensor-domain algorithm cannot reuse the same identity.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LocalDetailInput {
    LayerInputOklabLightness,
}

/// The exact local-detail statistic.
///
/// `BoxMeanAbsoluteResidualV1` converts the Grade Node input pixel to Oklab
/// lightness and computes a full-render-edge-clamped `(2r + 1)²` square box
/// mean. `r` is the persisted level-zero radius multiplied by the semantic
/// full-render scale, rounded half-up, with a one-output-pixel floor; tile
/// dimensions never participate. The absolute residual is normalized by
/// [`LOCAL_DETAIL_RESIDUAL_NORMALIZATION`]. The normative footprint, sample
/// order, and rounding are implemented by
/// [`super::local_detail_reference_response`].
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LocalDetailAlgorithm {
    BoxMeanAbsoluteResidualV1,
}

pub(super) fn validate_predicate(
    predicate: &ConditionMaskPredicate,
) -> Result<(), RecipeValidationError> {
    match predicate {
        ConditionMaskPredicate::OklabLightnessRange { lower, upper, .. } => {
            validate_range("Oklab lightness", *lower, *upper)
        }
        ConditionMaskPredicate::OklchHueRange {
            center_hue_degrees,
            half_width_degrees,
            minimum_chroma,
            minimum_chroma_feather,
            ..
        } => {
            if !(0.0..360.0).contains(&center_hue_degrees.get()) {
                return Err(RecipeValidationError::InvalidConditionMaskHue(
                    center_hue_degrees.get(),
                ));
            }
            if !(1.0..=180.0).contains(&half_width_degrees.get()) {
                return Err(RecipeValidationError::InvalidConditionMaskHueWidth(
                    half_width_degrees.get(),
                ));
            }
            if *minimum_chroma == UnitInterval::ZERO
                && *minimum_chroma_feather != UnitInterval::ZERO
            {
                return Err(RecipeValidationError::UnusedConditionMaskChromaFeather);
            }
            Ok(())
        }
        ConditionMaskPredicate::OklchChromaRange { lower, upper, .. } => {
            validate_range("normalized Oklch chroma", *lower, *upper)
        }
        ConditionMaskPredicate::LocalDetailRange {
            radius_level_zero_pixels,
            lower,
            upper,
            ..
        } => {
            if !(1..=MAX_LOCAL_DETAIL_RADIUS_LEVEL_ZERO_PIXELS).contains(radius_level_zero_pixels) {
                return Err(RecipeValidationError::InvalidLocalDetailMaskRadius(
                    *radius_level_zero_pixels,
                ));
            }
            validate_range("local detail", *lower, *upper)
        }
    }
}

const fn zero_unit_interval() -> UnitInterval {
    UnitInterval::ZERO
}

fn validate_range(
    kind: &'static str,
    lower: UnitInterval,
    upper: UnitInterval,
) -> Result<(), RecipeValidationError> {
    if lower > upper {
        Err(RecipeValidationError::InvalidConditionMaskRange {
            kind,
            lower: lower.get(),
            upper: upper.get(),
        })
    } else {
        Ok(())
    }
}
