use serde::{Deserialize, Serialize};
use shadow_domain::{GroupId, PhotoId};

use crate::{AiArtifactContractError, AiTaskParameters, ResourceEstimate, UnitInterval};

/// Replaceable AI capabilities. A provider advertises these rather than a vendor name.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AiCapability {
    TechnicalQuality,
    SimilarityEmbedding,
    BurstGrouping,
    PersonalRanking,
    AutoDevelopRecipe,
    SemanticEmbedding,
    SemanticCaption,
    SubjectMask,
    DepthEstimation,
    InpaintPatch,
    Denoise,
    SuperResolution,
    QueryPlanning,
}

/// One concrete unit of work submitted to an AI provider.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AiTaskKind {
    AssessTechnicalQuality,
    ExtractSimilarityEmbedding,
    ProposeBurstGroup,
    RankWithinGroup,
    ProposeDevelopRecipe,
    ExtractSemanticEmbedding,
    GenerateCaption,
    ProposeSubjectMask,
    EstimateDepth,
    GenerateInpaintPatch,
    Denoise,
    SuperResolve,
    PlanSearchQuery,
}

impl AiTaskKind {
    pub const fn capability(self) -> AiCapability {
        match self {
            Self::AssessTechnicalQuality => AiCapability::TechnicalQuality,
            Self::ExtractSimilarityEmbedding => AiCapability::SimilarityEmbedding,
            Self::ProposeBurstGroup => AiCapability::BurstGrouping,
            Self::RankWithinGroup => AiCapability::PersonalRanking,
            Self::ProposeDevelopRecipe => AiCapability::AutoDevelopRecipe,
            Self::ExtractSemanticEmbedding => AiCapability::SemanticEmbedding,
            Self::GenerateCaption => AiCapability::SemanticCaption,
            Self::ProposeSubjectMask => AiCapability::SubjectMask,
            Self::EstimateDepth => AiCapability::DepthEstimation,
            Self::GenerateInpaintPatch => AiCapability::InpaintPatch,
            Self::Denoise => AiCapability::Denoise,
            Self::SuperResolve => AiCapability::SuperResolution,
            Self::PlanSearchQuery => AiCapability::QueryPlanning,
        }
    }
}

/// Global work priority shared with render/import scheduling.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TaskPriority {
    CurrentInput,
    CurrentViewport,
    VisibleThumbnail,
    NearbyPrefetch,
    ImportValidation,
    CurrentCollectionAnalysis,
    UserExport,
    LibraryBackgroundAnalysis,
    CacheMaintenance,
}

impl TaskPriority {
    pub const fn is_interactive(self) -> bool {
        matches!(
            self,
            Self::CurrentInput | Self::CurrentViewport | Self::VisibleThumbnail
        )
    }

    pub const fn is_background(self) -> bool {
        matches!(
            self,
            Self::LibraryBackgroundAnalysis | Self::CacheMaintenance
        )
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PrivacyClass {
    Public,
    Personal,
    SensitiveBiometric,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum ObservationTarget {
    Photo {
        photo_id: PhotoId,
    },
    Group {
        group_id: GroupId,
    },
    Region {
        photo_id: PhotoId,
        region_id: String,
    },
    Library,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum InputRole {
    EmbeddedPreview,
    DisplayProxy,
    SceneLinearTile,
    CurrentRenderedCrop,
    Mask,
    FrozenFeatureVector,
    StructuredMetadata,
    SearchText,
}

/// A content-addressed input. The model worker never receives catalog write access.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ArtifactReference {
    pub role: InputRole,
    pub content_hash: String,
    pub byte_len: u64,
    pub media_type: String,
    pub privacy: PrivacyClass,
}

/// A serializable job envelope suitable for an eventual worker protocol.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AiJobRequest {
    pub request_id: String,
    /// Monotonically increasing UI/application generation used to discard stale results.
    pub generation: u64,
    pub task: AiTaskKind,
    pub target: ObservationTarget,
    pub model_id: String,
    pub model_revision: String,
    pub priority: TaskPriority,
    pub privacy: PrivacyClass,
    pub inputs: Vec<ArtifactReference>,
    #[serde(default)]
    pub parameters: AiTaskParameters,
    pub estimate: ResourceEstimate,
}

impl AiJobRequest {
    /// Validates task-specific parameters before a request crosses into a
    /// model worker.
    ///
    /// # Errors
    ///
    /// Returns an error when a task is paired with missing or unrelated typed
    /// parameters.
    pub fn validate_task_parameters(&self) -> Result<(), AiArtifactContractError> {
        self.parameters.validate_for(self.task)
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ModelProvenance {
    pub provider_id: String,
    pub model_id: String,
    pub model_revision: String,
    pub model_sha256: String,
    pub preprocessing_version: String,
    pub input_source_hash: String,
    pub cache_key: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ExplanationSignal {
    /// Stable machine-readable signal such as `eyes_open` or `highlight_clip_risk`.
    pub code: String,
    pub value: f64,
    /// Optional localized UI text is produced outside the worker from `code`.
    pub detail: Option<String>,
}

/// The most automation Shadow permits for a result.
///
/// `ProposalOnly` still creates an editable proposal/version; it never mutates an
/// accepted user version and never deletes or rejects an original asset.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ProposalReviewLevel {
    ObservationOnly,
    NeedsConfirmation,
    ProposalOnly,
}

impl ProposalReviewLevel {
    pub fn from_confidence(
        confidence: UnitInterval,
        confirm_threshold: UnitInterval,
        proposal_threshold: UnitInterval,
    ) -> Self {
        if confidence >= proposal_threshold {
            Self::ProposalOnly
        } else if confidence >= confirm_threshold {
            Self::NeedsConfirmation
        } else {
            Self::ObservationOnly
        }
    }
}

/// Unified model result. `payload` must be interpreted by the application core,
/// which alone may turn it into a catalog decision, mask, or edit-graph command.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AiObservation<T> {
    pub request_id: String,
    pub generation: u64,
    pub task: AiTaskKind,
    pub target: ObservationTarget,
    pub provenance: ModelProvenance,
    pub confidence: UnitInterval,
    pub review_level: ProposalReviewLevel,
    pub explanation_signals: Vec<ExplanationSignal>,
    pub payload: T,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn confidence_never_grants_direct_application() {
        assert_eq!(
            ProposalReviewLevel::from_confidence(
                UnitInterval::ONE,
                UnitInterval::new(0.5).expect("valid threshold"),
                UnitInterval::new(0.85).expect("valid threshold"),
            ),
            ProposalReviewLevel::ProposalOnly
        );
    }

    #[test]
    fn every_task_maps_to_a_provider_capability() {
        assert_eq!(
            AiTaskKind::GenerateInpaintPatch.capability(),
            AiCapability::InpaintPatch
        );
    }
}
