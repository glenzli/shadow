//! Provider-neutral intent for re-evaluable semantic local masks.

use serde::{Deserialize, Serialize};

use super::RecipeValidationError;

pub const SEMANTIC_MASK_INTENT_CONTRACT_VERSION: u32 = 1;
pub const MAX_SEMANTIC_MASK_QUERY_BYTES: usize = 256;
pub const MAX_SEMANTIC_MASK_REGIONS: u8 = 8;

/// How independently grounded regions contribute to one accepted mask.
#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SemanticMaskAggregation {
    /// Refine every grounded region and take the per-pixel maximum.
    #[default]
    Union,
}

/// Stable semantic intent attached to an accepted managed-raster mask.
///
/// The current raster remains the exact reproducible result for this photo.
/// Copying to another photo carries this intent and reruns grounding plus
/// segmentation there; it never reuses the source photo's raster pixels.
#[derive(Debug, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SemanticMaskIntent {
    contract_version: u32,
    query: String,
    query_revision: String,
    maximum_regions: u8,
    score_threshold_percent: u8,
    aggregation: SemanticMaskAggregation,
}

impl SemanticMaskIntent {
    /// Creates one canonical, bounded query intent.
    ///
    /// # Errors
    ///
    /// Returns an error for an empty/oversized query, an unsupported region
    /// count, or a score threshold outside 1% through 100%.
    pub fn new(
        query: impl Into<String>,
        maximum_regions: u8,
        score_threshold_percent: u8,
        aggregation: SemanticMaskAggregation,
    ) -> Result<Self, RecipeValidationError> {
        let query = canonical_query(&query.into());
        let query_revision = query_revision(&query);
        let intent = Self {
            contract_version: SEMANTIC_MASK_INTENT_CONTRACT_VERSION,
            query,
            query_revision,
            maximum_regions,
            score_threshold_percent,
            aggregation,
        };
        intent.validate()?;
        Ok(intent)
    }

    pub const fn contract_version(&self) -> u32 {
        self.contract_version
    }

    pub fn query(&self) -> &str {
        &self.query
    }

    pub fn query_revision(&self) -> &str {
        &self.query_revision
    }

    pub const fn maximum_regions(&self) -> u8 {
        self.maximum_regions
    }

    pub const fn score_threshold_percent(&self) -> u8 {
        self.score_threshold_percent
    }

    pub const fn aggregation(&self) -> SemanticMaskAggregation {
        self.aggregation
    }

    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        if self.contract_version != SEMANTIC_MASK_INTENT_CONTRACT_VERSION {
            return Err(
                RecipeValidationError::UnsupportedSemanticMaskIntentVersion {
                    expected: SEMANTIC_MASK_INTENT_CONTRACT_VERSION,
                    actual: self.contract_version,
                },
            );
        }
        if self.query.is_empty() {
            return Err(RecipeValidationError::EmptySemanticMaskQuery);
        }
        if self.query.len() > MAX_SEMANTIC_MASK_QUERY_BYTES {
            return Err(RecipeValidationError::SemanticMaskQueryTooLong {
                actual_bytes: self.query.len(),
                max_bytes: MAX_SEMANTIC_MASK_QUERY_BYTES,
            });
        }
        if canonical_query(&self.query) != self.query {
            return Err(RecipeValidationError::NonCanonicalSemanticMaskQuery);
        }
        if self.query_revision != query_revision(&self.query) {
            return Err(RecipeValidationError::InvalidSemanticMaskQueryRevision);
        }
        if !(1..=MAX_SEMANTIC_MASK_REGIONS).contains(&self.maximum_regions) {
            return Err(RecipeValidationError::InvalidSemanticMaskMaximumRegions(
                self.maximum_regions,
            ));
        }
        if !(1..=100).contains(&self.score_threshold_percent) {
            return Err(RecipeValidationError::InvalidSemanticMaskScoreThreshold(
                self.score_threshold_percent,
            ));
        }
        Ok(())
    }
}

fn canonical_query(query: &str) -> String {
    query.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn query_revision(query: &str) -> String {
    format!(
        "shadow-semantic-mask-v1:{}",
        blake3::hash(query.as_bytes()).to_hex()
    )
}
