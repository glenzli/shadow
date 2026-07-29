//! Bounded, renderer-neutral pixel conditions and copy-only presets.
//!
//! Follow `expression` for the recursive grammar and aggregate bounds,
//! `predicate` for persisted leaf parameters, `reference` for the exact scalar
//! coverage and local-detail footprint contract, and `preset` for the reusable
//! authoring value whose parameters are copied into a Recipe.

mod expression;
mod predicate;
mod preset;
mod reference;

pub(crate) use expression::LegacyConditionLeaf;
pub use expression::{
    CURRENT_CONDITION_MASK_SCHEMA_VERSION, ConditionMaskExpression, ConditionMaskNode,
    MAX_CONDITION_MASK_BRANCHES, MAX_CONDITION_MASK_DEPTH, MAX_CONDITION_MASK_LEAVES,
};
pub use predicate::{
    ConditionMaskPredicate, LOCAL_DETAIL_RESIDUAL_NORMALIZATION, LocalDetailAlgorithm,
    LocalDetailInput, MAX_LOCAL_DETAIL_RADIUS_LEVEL_ZERO_PIXELS, OKLCH_CHROMA_NORMALIZATION,
};
pub use preset::ConditionMaskPreset;
pub use reference::{
    CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_LOWER, CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_UPPER,
    ConditionMaskReferenceError, ConditionMaskScalarSample, LocalDetailFullRenderScale,
    local_detail_reference_response,
};

#[cfg(test)]
mod tests;
