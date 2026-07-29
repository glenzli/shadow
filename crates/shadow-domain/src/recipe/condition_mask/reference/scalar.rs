//! Scalar expression, range, hue-confidence, and chroma-gate formulas.

use crate::recipe::{FiniteF64, RecipeValidationError, UnitInterval};

use super::super::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, OKLCH_CHROMA_NORMALIZATION,
};

/// Lower relative Oklab-chroma boundary of the legacy hue-confidence curve.
pub const CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_LOWER: f64 = 0.002;
/// Upper relative Oklab-chroma boundary of the legacy hue-confidence curve.
pub const CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_UPPER: f64 = 0.02;
const OKLAB_HUE_LIGHTNESS_DENOMINATOR_FLOOR: f64 = 1.0e-6;

/// One finite Grade Node input sample plus the separately prepared local-detail
/// response for the same full-render pixel.
#[derive(Debug, Copy, Clone, PartialEq)]
pub struct ConditionMaskScalarSample {
    oklab_lightness: FiniteF64,
    oklab_a: FiniteF64,
    oklab_b: FiniteF64,
    local_detail_response: UnitInterval,
}

impl ConditionMaskScalarSample {
    /// Creates one reference-evaluation sample.
    ///
    /// `local_detail_response` must come from
    /// [`super::local_detail_reference_response`] when the expression contains
    /// a local-detail predicate.
    ///
    /// # Errors
    ///
    /// Returns an error if any Oklab component is non-finite.
    pub fn new(
        oklab_lightness: f64,
        oklab_a: f64,
        oklab_b: f64,
        local_detail_response: UnitInterval,
    ) -> Result<Self, RecipeValidationError> {
        Ok(Self {
            oklab_lightness: FiniteF64::new(oklab_lightness)?,
            oklab_a: FiniteF64::new(oklab_a)?,
            oklab_b: FiniteF64::new(oklab_b)?,
            local_detail_response,
        })
    }

    pub const fn oklab_lightness(self) -> f64 {
        self.oklab_lightness.get()
    }

    pub const fn oklab_a(self) -> f64 {
        self.oklab_a.get()
    }

    pub const fn oklab_b(self) -> f64 {
        self.oklab_b.get()
    }

    pub const fn local_detail_response(self) -> UnitInterval {
        self.local_detail_response
    }
}

impl ConditionMaskExpression {
    /// Evaluates this expression with the normative scalar formulas.
    ///
    /// The result is always in `[0, 1]`. This is a parity reference, not a
    /// performance-oriented full-image renderer.
    pub fn reference_coverage(&self, sample: ConditionMaskScalarSample) -> f64 {
        reference_node_coverage(self.root(), sample)
    }
}

impl ConditionMaskPredicate {
    /// Evaluates this leaf with the normative scalar formulas.
    pub fn reference_coverage(&self, sample: ConditionMaskScalarSample) -> f64 {
        let lightness = sample.oklab_lightness();
        let a = sample.oklab_a();
        let b = sample.oklab_b();
        let chroma = a.hypot(b);
        match self {
            Self::OklabLightnessRange {
                lower,
                upper,
                softness,
            } => range_coverage(lightness, *lower, *upper, *softness),
            Self::OklchHueRange {
                center_hue_degrees,
                half_width_degrees,
                minimum_chroma,
                minimum_chroma_feather,
                softness,
            } => {
                let relative_chroma =
                    chroma / lightness.abs().max(OKLAB_HUE_LIGHTNESS_DENOMINATOR_FLOOR);
                let confidence = smoothstep_between(
                    CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_LOWER,
                    CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_UPPER,
                    relative_chroma,
                );
                let hue = b.atan2(a).to_degrees().rem_euclid(360.0);
                let hue_weight = hue_range_coverage(
                    hue,
                    center_hue_degrees.get(),
                    half_width_degrees.get(),
                    softness.get(),
                );
                let normalized_chroma = (chroma / OKLCH_CHROMA_NORMALIZATION).clamp(0.0, 1.0);
                let absolute_chroma_gate = minimum_chroma_gate(
                    normalized_chroma,
                    minimum_chroma.get(),
                    minimum_chroma_feather.get(),
                );
                (confidence * hue_weight * absolute_chroma_gate).clamp(0.0, 1.0)
            }
            Self::OklchChromaRange {
                lower,
                upper,
                softness,
            } => range_coverage(
                chroma / OKLCH_CHROMA_NORMALIZATION,
                *lower,
                *upper,
                *softness,
            ),
            Self::LocalDetailRange {
                lower,
                upper,
                softness,
                ..
            } => range_coverage(
                sample.local_detail_response().get(),
                *lower,
                *upper,
                *softness,
            ),
        }
    }
}

fn reference_node_coverage(node: &ConditionMaskNode, sample: ConditionMaskScalarSample) -> f64 {
    match node {
        ConditionMaskNode::Leaf { condition } => condition.reference_coverage(sample),
        ConditionMaskNode::All { children } => children
            .iter()
            .map(|child| reference_node_coverage(child, sample))
            .fold(1.0, f64::min),
        ConditionMaskNode::Any { children } => children
            .iter()
            .map(|child| reference_node_coverage(child, sample))
            .fold(0.0, f64::max),
        ConditionMaskNode::Not { child } => 1.0 - reference_node_coverage(child, sample),
    }
}

fn range_coverage(
    value: f64,
    lower: UnitInterval,
    upper: UnitInterval,
    softness: UnitInterval,
) -> f64 {
    let value = value.clamp(0.0, 1.0);
    if softness == UnitInterval::ZERO {
        return if value >= lower.get() && value <= upper.get() {
            1.0
        } else {
            0.0
        };
    }
    let softness = softness.get();
    let lower_weight = smootherstep((value - (lower.get() - softness)) / softness);
    let upper_weight = 1.0 - smootherstep((value - upper.get()) / softness);
    lower_weight.min(upper_weight)
}

fn hue_range_coverage(
    hue: f64,
    center_degrees: f64,
    half_width_degrees: f64,
    softness: f64,
) -> f64 {
    let distance = ((hue - center_degrees + 180.0).rem_euclid(360.0) - 180.0).abs();
    let feather = half_width_degrees * softness;
    if feather <= 0.0 {
        return if distance <= half_width_degrees {
            1.0
        } else {
            0.0
        };
    }
    1.0 - smoothstep_between(half_width_degrees - feather, half_width_degrees, distance)
}

fn minimum_chroma_gate(normalized_chroma: f64, minimum_chroma: f64, feather: f64) -> f64 {
    if minimum_chroma == 0.0 {
        return 1.0;
    }
    if feather == 0.0 {
        return if normalized_chroma >= minimum_chroma {
            1.0
        } else {
            0.0
        };
    }
    smoothstep_between(
        (minimum_chroma - feather).max(0.0),
        minimum_chroma,
        normalized_chroma,
    )
}

fn smootherstep(value: f64) -> f64 {
    let x = value.clamp(0.0, 1.0);
    x * x * x * (x * (x * 6.0 - 15.0) + 10.0)
}

fn smoothstep_between(lower: f64, upper: f64, value: f64) -> f64 {
    if value <= lower {
        return 0.0;
    }
    if value >= upper {
        return 1.0;
    }
    let normalized = (value - lower) / (upper - lower);
    normalized * normalized * (3.0 - 2.0 * normalized)
}

#[cfg(test)]
mod tests;
