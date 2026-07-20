use serde::{Deserialize, Serialize};
use thiserror::Error;

/// A finite scalar in the inclusive range `0.0..=1.0`.
///
/// Confidence and normalized signals use this type so invalid model output is
/// rejected at the worker boundary instead of silently contaminating ranking.
#[derive(Debug, Copy, Clone, PartialEq, PartialOrd, Serialize, Deserialize)]
#[serde(try_from = "f64", into = "f64")]
pub struct UnitInterval(f64);

impl UnitInterval {
    pub const ZERO: Self = Self(0.0);
    pub const ONE: Self = Self(1.0);

    /// Creates a unit interval value.
    ///
    /// # Errors
    ///
    /// Returns [`UnitIntervalError`] for NaN, infinity, or an out-of-range value.
    pub fn new(value: f64) -> Result<Self, UnitIntervalError> {
        if !value.is_finite() {
            return Err(UnitIntervalError::NotFinite);
        }
        if !(0.0..=1.0).contains(&value) {
            return Err(UnitIntervalError::OutOfRange(value));
        }
        Ok(Self(value))
    }

    pub const fn get(self) -> f64 {
        self.0
    }

    pub(crate) fn weighted_average(parts: &[(Self, f64)]) -> Self {
        let (weighted_sum, weight_sum) = parts
            .iter()
            .filter(|(_, weight)| weight.is_finite() && *weight > 0.0)
            .fold((0.0, 0.0), |(sum, total), (value, weight)| {
                (sum + value.get() * weight, total + weight)
            });
        if weight_sum == 0.0 {
            Self::ZERO
        } else {
            // Inputs are unit values and weights are positive, so the result is
            // guaranteed to remain in range apart from harmless FP drift.
            Self((weighted_sum / weight_sum).clamp(0.0, 1.0))
        }
    }
}

impl Default for UnitInterval {
    fn default() -> Self {
        Self::ZERO
    }
}

impl TryFrom<f64> for UnitInterval {
    type Error = UnitIntervalError;

    fn try_from(value: f64) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<UnitInterval> for f64 {
    fn from(value: UnitInterval) -> Self {
        value.get()
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Error)]
pub enum UnitIntervalError {
    #[error("unit interval value must be finite")]
    NotFinite,
    #[error("unit interval value must be between 0 and 1, got {0}")]
    OutOfRange(f64),
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serde_rejects_invalid_confidence() {
        let result = serde_json::from_str::<UnitInterval>("1.2");
        assert!(result.is_err());
    }

    #[test]
    fn weighted_average_ignores_zero_weight() {
        let result = UnitInterval::weighted_average(&[
            (UnitInterval::ONE, 0.0),
            (UnitInterval::new(0.25).expect("valid score"), 2.0),
        ]);
        assert_eq!(result, UnitInterval::new(0.25).expect("valid score"));
    }
}
