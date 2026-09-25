//! Immutable generation-time source calibration; contains no image or host path.
use crate::{PhotoFoundationNode, RawFoundationDenoise, RecipeValidationError};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ImageCompletionColorBasis {
    /// IEEE-754 bits preserve exact finite matrix identity across Recipe JSON.
    pub matrix_bits: [u64; 9],
    pub calibration_id: String,
}
impl ImageCompletionColorBasis {
    pub fn matrix(&self) -> [f64; 9] {
        self.matrix_bits.map(f64::from_bits)
    }
    pub fn valid(&self) -> bool {
        let m = self.matrix();
        let det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6])
            + m[2] * (m[3] * m[7] - m[4] * m[6]);
        !self.calibration_id.is_empty()
            && self.calibration_id.len() <= 1024
            && m.iter().all(|v| v.is_finite() && v.abs() <= 256.0)
            && det.is_finite()
            && det.abs() > 1e-10
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ImageCompletionSourceContext {
    pub color_basis: Option<ImageCompletionColorBasis>,
    pub foundation: PhotoFoundationNode,
    pub raw_ai_denoise: RawFoundationDenoise,
}
impl ImageCompletionSourceContext {
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        self.foundation.validate()?;
        self.raw_ai_denoise.validate()?;
        if self.color_basis.as_ref().is_some_and(|b| !b.valid()) {
            return Err(RecipeValidationError::InvalidImageCompletionSourceIdentity);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests;
