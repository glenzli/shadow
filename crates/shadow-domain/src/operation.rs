//! Stable identifiers shared by persisted Recipes and operation executors.
//!
//! Changing one of these strings changes the persisted operation contract. A
//! different parameter shape or implementation therefore needs a new schema
//! or implementation version instead of silently reusing an existing value.

/// Parameter schema used by the current CPU reference operations.
pub const CPU_REFERENCE_PARAMETER_SCHEMA_VERSION: u32 = 1;

/// Stable implementation identifier used by the current CPU reference executor.
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

pub const CHANNEL_GAIN_OPERATION_ID: &str = "shadow.channel_gain";
pub const CHANNEL_GAINS_PARAMETER_KEY: &str = "channel_gains";

pub const SATURATION_OPERATION_ID: &str = "shadow.saturation";
pub const SATURATION_FACTOR_PARAMETER_KEY: &str = "factor";

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
            CHANNEL_GAIN_OPERATION_ID,
            SATURATION_OPERATION_ID,
        ] {
            OperationId::new(operation_id).expect("operation contract id must remain valid");
        }

        for parameter_key in [
            EXPOSURE_STOPS_PARAMETER_KEY,
            CONTRAST_FACTOR_PARAMETER_KEY,
            CONTRAST_PIVOT_PARAMETER_KEY,
            TONE_CURVE_POINTS_PARAMETER_KEY,
            CHANNEL_GAINS_PARAMETER_KEY,
            SATURATION_FACTOR_PARAMETER_KEY,
        ] {
            ParameterKey::new(parameter_key)
                .expect("operation contract parameter key must remain valid");
        }
    }
}
