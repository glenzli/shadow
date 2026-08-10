//! Provider-neutral semantic image evidence for local search and keyword review.
//!
//! Image/text embeddings are comparable only inside one exact space. Structured
//! analysis remains a rebuildable proposal: it can suggest bounded concepts and
//! a short caption, but it cannot create a committed Library keyword.

use std::{collections::BTreeSet, fmt};

use serde::{Deserialize, Deserializer, Serialize, de::Error as _};
use thiserror::Error;

pub const SEMANTIC_EMBEDDING_CONTRACT_VERSION: u32 = 1;
pub const SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION: u32 = 1;
pub const MAX_SEMANTIC_EMBEDDING_DIMENSIONS: usize = 4_096;
pub const MAX_SEMANTIC_KEYWORD_SUGGESTIONS: usize = 64;
pub const MAX_SEMANTIC_ID_BYTES: usize = 256;
pub const MAX_SEMANTIC_LABEL_BYTES: usize = 160;
pub const MAX_SHORT_CAPTION_BYTES: usize = 1_024;
pub const MAX_LANGUAGE_TAG_BYTES: usize = 35;

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize)]
pub struct SemanticEmbeddingSpace {
    contract_version: u32,
    space_id: String,
    dimensions: u16,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SemanticEmbeddingSpaceWire {
    contract_version: u32,
    space_id: String,
    dimensions: u16,
}

impl SemanticEmbeddingSpace {
    /// Creates one exact image/text embedding space identity.
    ///
    /// # Errors
    ///
    /// Rejects unsupported contracts, unversioned identities, or dimensions
    /// outside the bounded persistence/search contract.
    pub fn new(
        contract_version: u32,
        space_id: impl Into<String>,
        dimensions: usize,
    ) -> Result<Self, SemanticContractError> {
        if contract_version != SEMANTIC_EMBEDDING_CONTRACT_VERSION {
            return Err(SemanticContractError::UnsupportedEmbeddingContract(
                contract_version,
            ));
        }
        let space_id = space_id.into();
        if !valid_portable_id(&space_id) {
            return Err(SemanticContractError::InvalidEmbeddingSpace);
        }
        if dimensions == 0 || dimensions > MAX_SEMANTIC_EMBEDDING_DIMENSIONS {
            return Err(SemanticContractError::InvalidEmbeddingDimensions(
                dimensions,
            ));
        }
        let bounded_dimensions = u16::try_from(dimensions)
            .map_err(|_| SemanticContractError::InvalidEmbeddingDimensions(dimensions))?;
        Ok(Self {
            contract_version,
            space_id,
            dimensions: bounded_dimensions,
        })
    }

    pub const fn contract_version(&self) -> u32 {
        self.contract_version
    }

    pub fn space_id(&self) -> &str {
        &self.space_id
    }

    pub const fn dimensions(&self) -> usize {
        self.dimensions as usize
    }
}

impl<'de> Deserialize<'de> for SemanticEmbeddingSpace {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = SemanticEmbeddingSpaceWire::deserialize(deserializer)?;
        Self::new(
            wire.contract_version,
            wire.space_id,
            usize::from(wire.dimensions),
        )
        .map_err(D::Error::custom)
    }
}

#[derive(Clone, PartialEq)]
pub struct SemanticEmbedding {
    space: SemanticEmbeddingSpace,
    values: Box<[f32]>,
}

impl fmt::Debug for SemanticEmbedding {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("SemanticEmbedding")
            .field("values", &"<personal-semantic-vector-redacted>")
            .field("space", &self.space)
            .finish()
    }
}

impl SemanticEmbedding {
    /// Admits one finite, L2-normalized image or text vector.
    ///
    /// # Errors
    ///
    /// Rejects dimension mismatch, non-finite values, and vectors outside the
    /// normalization tolerance used by the typed Runtime contract.
    pub fn new(
        space: SemanticEmbeddingSpace,
        values: Vec<f32>,
    ) -> Result<Self, SemanticContractError> {
        if values.len() != space.dimensions() {
            return Err(SemanticContractError::EmbeddingDimensionMismatch {
                expected: space.dimensions(),
                actual: values.len(),
            });
        }
        if values.iter().any(|value| !value.is_finite()) {
            return Err(SemanticContractError::InvalidEmbeddingValue);
        }
        let squared_norm = values
            .iter()
            .map(|value| f64::from(*value) * f64::from(*value))
            .sum::<f64>();
        if (squared_norm.sqrt() - 1.0).abs() > 1.0e-3 {
            return Err(SemanticContractError::EmbeddingNotNormalized);
        }
        Ok(Self {
            space,
            values: values.into_boxed_slice(),
        })
    }

    pub const fn space(&self) -> &SemanticEmbeddingSpace {
        &self.space
    }

    pub fn values(&self) -> &[f32] {
        &self.values
    }

    /// Returns cosine similarity only for vectors in the same exact space.
    pub fn cosine_similarity(&self, other: &Self) -> Option<f32> {
        if self.space != other.space {
            return None;
        }
        Some(
            self.values
                .iter()
                .zip(other.values.iter())
                .map(|(left, right)| left * right)
                .sum::<f32>()
                .clamp(-1.0, 1.0),
        )
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SemanticKeywordKind {
    /// Open-ended model concept whose narrower ontology is intentionally
    /// unknown to the Consumer.
    Concept,
    Scene,
    Object,
    Activity,
    Attribute,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SemanticEvidenceKind {
    DirectObservation,
    ModelInterpretation,
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SemanticKeywordSuggestion {
    pub kind: SemanticKeywordKind,
    pub concept_id: String,
    pub display_label: String,
    pub evidence: SemanticEvidenceKind,
}

impl SemanticKeywordSuggestion {
    fn validate(&self) -> Result<(), SemanticContractError> {
        if !valid_portable_id(&self.concept_id) {
            return Err(SemanticContractError::InvalidConceptId);
        }
        if !valid_display_text(&self.display_label, MAX_SEMANTIC_LABEL_BYTES) {
            return Err(SemanticContractError::InvalidConceptLabel);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SemanticShortCaption {
    pub language_tag: String,
    pub text: String,
}

impl SemanticShortCaption {
    fn validate(&self) -> Result<(), SemanticContractError> {
        if self.language_tag.is_empty()
            || self.language_tag.len() > MAX_LANGUAGE_TAG_BYTES
            || !self
                .language_tag
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
        {
            return Err(SemanticContractError::InvalidLanguageTag);
        }
        if !valid_display_text(&self.text, MAX_SHORT_CAPTION_BYTES) {
            return Err(SemanticContractError::InvalidShortCaption);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
pub struct SemanticImageAnalysis {
    pub schema_version: u32,
    pub analyzer_revision: String,
    pub prompt_revision: String,
    pub suggestions: Vec<SemanticKeywordSuggestion>,
    pub short_caption: SemanticShortCaption,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SemanticImageAnalysisWire {
    schema_version: u32,
    analyzer_revision: String,
    prompt_revision: String,
    #[serde(deserialize_with = "deserialize_suggestions")]
    suggestions: Vec<SemanticKeywordSuggestion>,
    short_caption: SemanticShortCaption,
}

impl SemanticImageAnalysis {
    /// Creates a bounded, rebuildable image-understanding proposal.
    ///
    /// # Errors
    ///
    /// Rejects unsupported schema revisions, malformed provenance, duplicate
    /// concepts, unbounded output, or invalid localized text.
    pub fn new(
        schema_version: u32,
        analyzer_revision: impl Into<String>,
        prompt_revision: impl Into<String>,
        suggestions: Vec<SemanticKeywordSuggestion>,
        short_caption: SemanticShortCaption,
    ) -> Result<Self, SemanticContractError> {
        if schema_version != SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION {
            return Err(SemanticContractError::UnsupportedAnalysisSchema(
                schema_version,
            ));
        }
        let analyzer_revision = analyzer_revision.into();
        let prompt_revision = prompt_revision.into();
        if !valid_portable_id(&analyzer_revision) || !valid_portable_id(&prompt_revision) {
            return Err(SemanticContractError::InvalidAnalysisRevision);
        }
        if suggestions.len() > MAX_SEMANTIC_KEYWORD_SUGGESTIONS {
            return Err(SemanticContractError::TooManyKeywordSuggestions(
                suggestions.len(),
            ));
        }
        let mut identities = BTreeSet::new();
        for suggestion in &suggestions {
            suggestion.validate()?;
            if !identities.insert((suggestion.kind, suggestion.concept_id.as_str())) {
                return Err(SemanticContractError::DuplicateKeywordSuggestion);
            }
        }
        short_caption.validate()?;
        Ok(Self {
            schema_version,
            analyzer_revision,
            prompt_revision,
            suggestions,
            short_caption,
        })
    }
}

impl<'de> Deserialize<'de> for SemanticImageAnalysis {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = SemanticImageAnalysisWire::deserialize(deserializer)?;
        Self::new(
            wire.schema_version,
            wire.analyzer_revision,
            wire.prompt_revision,
            wire.suggestions,
            wire.short_caption,
        )
        .map_err(D::Error::custom)
    }
}

fn deserialize_suggestions<'de, D>(
    deserializer: D,
) -> Result<Vec<SemanticKeywordSuggestion>, D::Error>
where
    D: Deserializer<'de>,
{
    let suggestions = Vec::<SemanticKeywordSuggestion>::deserialize(deserializer)?;
    if suggestions.len() > MAX_SEMANTIC_KEYWORD_SUGGESTIONS {
        return Err(D::Error::custom("too many semantic keyword suggestions"));
    }
    Ok(suggestions)
}

fn valid_portable_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_SEMANTIC_ID_BYTES
        && value.bytes().all(|byte| {
            byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_' | b':' | b'@')
        })
}

fn valid_display_text(value: &str, maximum_bytes: usize) -> bool {
    !value.trim().is_empty()
        && value.len() <= maximum_bytes
        && value == value.trim()
        && !value.chars().any(char::is_control)
}

#[derive(Debug, Error, Clone, Eq, PartialEq)]
pub enum SemanticContractError {
    #[error("unsupported semantic embedding contract {0}")]
    UnsupportedEmbeddingContract(u32),
    #[error("semantic embedding space identity is invalid")]
    InvalidEmbeddingSpace,
    #[error("semantic embedding dimension {0} is invalid")]
    InvalidEmbeddingDimensions(usize),
    #[error("semantic embedding dimension mismatch: expected {expected}, received {actual}")]
    EmbeddingDimensionMismatch { expected: usize, actual: usize },
    #[error("semantic embedding contains a non-finite value")]
    InvalidEmbeddingValue,
    #[error("semantic embedding is not L2-normalized")]
    EmbeddingNotNormalized,
    #[error("unsupported semantic image-analysis schema {0}")]
    UnsupportedAnalysisSchema(u32),
    #[error("semantic analysis revision identity is invalid")]
    InvalidAnalysisRevision,
    #[error("semantic keyword concept identity is invalid")]
    InvalidConceptId,
    #[error("semantic keyword display label is invalid")]
    InvalidConceptLabel,
    #[error("semantic analysis contains too many keyword suggestions: {0}")]
    TooManyKeywordSuggestions(usize),
    #[error("semantic analysis contains a duplicate keyword suggestion")]
    DuplicateKeywordSuggestion,
    #[error("semantic caption language tag is invalid")]
    InvalidLanguageTag,
    #[error("semantic short caption is invalid")]
    InvalidShortCaption,
}

#[cfg(test)]
mod tests;
