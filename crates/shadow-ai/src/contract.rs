use serde::{Deserialize, Deserializer, Serialize, de::Error as _};
use shadow_domain::{GroupId, PhotoId};
use thiserror::Error;

use crate::{
    AiArtifactContractError, AiTaskParameters, ResourceEstimate, UnitInterval,
    runtime::{LeaseBoundOutput, ModelProvenance, RuntimeContractError},
};

pub const AI_JOB_REQUEST_CONTRACT_VERSION: u32 = 1;
pub const AI_OBSERVATION_CONTRACT_VERSION: u32 = 1;
pub const MAX_EXPLANATION_SIGNALS: usize = 64;

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
#[serde(rename_all = "snake_case", tag = "kind", deny_unknown_fields)]
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
    /// Original encoded camera file. Local decoding only; never remotely admissible.
    RawFile,
    /// Unrendered CFA/sensor samples. Local execution only.
    SensorMosaic,
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
#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
pub struct ArtifactReference {
    pub role: InputRole,
    pub content_hash: String,
    pub byte_len: u64,
    pub media_type: String,
    pub privacy: PrivacyClass,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ArtifactReferenceWire {
    role: InputRole,
    content_hash: String,
    byte_len: u64,
    media_type: String,
    privacy: PrivacyClass,
}

impl<'de> Deserialize<'de> for ArtifactReference {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = ArtifactReferenceWire::deserialize(deserializer)?;
        if wire.byte_len == 0
            || wire.media_type.trim().is_empty()
            || wire.media_type.len() > 256
            || wire.content_hash.len() != 64
            || !wire
                .content_hash
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        {
            return Err(D::Error::custom("invalid AI input artifact identity"));
        }
        Ok(Self {
            role: wire.role,
            content_hash: wire.content_hash,
            byte_len: wire.byte_len,
            media_type: wire.media_type,
            privacy: wire.privacy,
        })
    }
}

/// A serializable user/application intent suitable for a worker protocol.
///
/// The request deliberately does not select a provider, model, checkpoint, or
/// remote service. Runtime admission binds this intent to one exact execution
/// route after privacy, license, availability, and resource policy have passed.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct AiJobRequest {
    pub contract_version: u32,
    pub request_id: String,
    /// Monotonically increasing UI/application generation used to discard stale results.
    pub generation: u64,
    pub task: AiTaskKind,
    pub target: ObservationTarget,
    pub priority: TaskPriority,
    pub privacy: PrivacyClass,
    pub inputs: Vec<ArtifactReference>,
    pub parameters: AiTaskParameters,
    /// Caller scheduling hint only.
    ///
    /// Hard memory/thread admission uses the selected provider route's
    /// independently supplied estimate; this hint is never the sole safety
    /// budget.
    pub estimate: ResourceEstimate,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AiJobRequestWire {
    contract_version: u32,
    request_id: String,
    generation: u64,
    task: AiTaskKind,
    target: ObservationTarget,
    priority: TaskPriority,
    privacy: PrivacyClass,
    #[serde(deserialize_with = "crate::wire_v1::vec_64")]
    inputs: Vec<ArtifactReference>,
    parameters: AiTaskParameters,
    estimate: ResourceEstimate,
}

impl<'de> Deserialize<'de> for AiJobRequest {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = AiJobRequestWire::deserialize(deserializer)?;
        if wire.contract_version != AI_JOB_REQUEST_CONTRACT_VERSION {
            return Err(D::Error::custom(format_args!(
                "AI request contract version {} is unsupported",
                wire.contract_version
            )));
        }
        if wire.request_id.trim().is_empty() {
            return Err(D::Error::custom("AI request_id must not be empty"));
        }
        if wire.inputs.iter().any(|input| input.privacy > wire.privacy) {
            return Err(D::Error::custom(
                "AI request privacy understates an input artifact",
            ));
        }
        let request = Self {
            contract_version: wire.contract_version,
            request_id: wire.request_id,
            generation: wire.generation,
            task: wire.task,
            target: wire.target,
            priority: wire.priority,
            privacy: wire.privacy,
            inputs: wire.inputs,
            parameters: wire.parameters,
            estimate: wire.estimate,
        };
        request
            .validate_task_parameters()
            .map_err(D::Error::custom)?;
        Ok(request)
    }
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

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
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
///
/// Runtime identity fields deliberately remain visible in the wire value for
/// auditability, but they are copied only from a consumed
/// [`LeaseBoundOutput`] and must exactly match its [`ModelProvenance`].
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct AiObservation<T> {
    contract_version: u32,
    request_id: String,
    generation: u64,
    task: AiTaskKind,
    target: ObservationTarget,
    provenance: ModelProvenance,
    confidence: UnitInterval,
    review_level: ProposalReviewLevel,
    explanation_signals: Vec<ExplanationSignal>,
    payload: T,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields, bound(deserialize = "T: Deserialize<'de>"))]
struct AiObservationWire<T> {
    contract_version: u32,
    request_id: String,
    generation: u64,
    task: AiTaskKind,
    target: ObservationTarget,
    provenance: ModelProvenance,
    confidence: UnitInterval,
    review_level: ProposalReviewLevel,
    #[serde(deserialize_with = "crate::wire_v1::vec_64")]
    explanation_signals: Vec<ExplanationSignal>,
    payload: T,
}

impl<'de, T> Deserialize<'de> for AiObservation<T>
where
    T: Deserialize<'de>,
{
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = AiObservationWire::deserialize(deserializer)?;
        let observation = Self {
            contract_version: wire.contract_version,
            request_id: wire.request_id,
            generation: wire.generation,
            task: wire.task,
            target: wire.target,
            provenance: wire.provenance,
            confidence: wire.confidence,
            review_level: wire.review_level,
            explanation_signals: wire.explanation_signals,
            payload: wire.payload,
        };
        observation.validate().map_err(D::Error::custom)?;
        Ok(observation)
    }
}

impl<T> AiObservation<T> {
    pub const fn contract_version(&self) -> u32 {
        self.contract_version
    }

    pub fn request_id(&self) -> &str {
        &self.request_id
    }

    pub const fn generation(&self) -> u64 {
        self.generation
    }

    pub const fn task(&self) -> AiTaskKind {
        self.task
    }

    pub const fn target(&self) -> &ObservationTarget {
        &self.target
    }

    pub const fn provenance(&self) -> &ModelProvenance {
        &self.provenance
    }

    pub const fn confidence(&self) -> UnitInterval {
        self.confidence
    }

    pub const fn review_level(&self) -> ProposalReviewLevel {
        self.review_level
    }

    pub fn explanation_signals(&self) -> &[ExplanationSignal] {
        &self.explanation_signals
    }

    pub const fn payload(&self) -> &T {
        &self.payload
    }

    pub fn into_payload(self) -> T {
        self.payload
    }

    pub(crate) fn from_runtime_output(
        completed: LeaseBoundOutput<T>,
        confidence: UnitInterval,
        review_level: ProposalReviewLevel,
        explanation_signals: Vec<ExplanationSignal>,
    ) -> Result<Self, AiObservationContractError> {
        let (payload, provenance) = completed.into_parts();
        let observation = Self {
            contract_version: AI_OBSERVATION_CONTRACT_VERSION,
            request_id: provenance.request_id().to_owned(),
            generation: provenance.generation(),
            task: provenance.task(),
            target: provenance.target().clone(),
            provenance,
            confidence,
            review_level,
            explanation_signals,
            payload,
        };
        observation.validate()?;
        Ok(observation)
    }

    /// Validates the exact v1 result envelope and duplicated runtime identity.
    ///
    /// # Errors
    ///
    /// Returns an error for an unsupported version, invalid provenance,
    /// request/target identity mismatch, or malformed explanation inventory.
    pub fn validate(&self) -> Result<(), AiObservationContractError> {
        if self.contract_version != AI_OBSERVATION_CONTRACT_VERSION {
            return Err(AiObservationContractError::UnsupportedContractVersion(
                self.contract_version,
            ));
        }
        self.provenance.validate()?;
        if self.request_id != self.provenance.request_id() {
            return Err(AiObservationContractError::IdentityMismatch("request_id"));
        }
        if self.generation != self.provenance.generation() {
            return Err(AiObservationContractError::IdentityMismatch("generation"));
        }
        if self.task != self.provenance.task() {
            return Err(AiObservationContractError::IdentityMismatch("task"));
        }
        if self.target != *self.provenance.target() {
            return Err(AiObservationContractError::IdentityMismatch("target"));
        }
        if self.explanation_signals.len() > MAX_EXPLANATION_SIGNALS {
            return Err(AiObservationContractError::TooManyExplanationSignals(
                self.explanation_signals.len(),
            ));
        }
        for (index, signal) in self.explanation_signals.iter().enumerate() {
            if signal.code.trim().is_empty() {
                return Err(AiObservationContractError::MissingExplanationCode(index));
            }
            if !signal.value.is_finite() {
                return Err(AiObservationContractError::NonFiniteExplanationValue(index));
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum AiObservationContractError {
    #[error("AI observation contract version {0} is unsupported")]
    UnsupportedContractVersion(u32),
    #[error("AI observation field {0} does not match runtime provenance")]
    IdentityMismatch(&'static str),
    #[error(
        "AI observation has {0} explanation signals; the v1 contract permits at most {MAX_EXPLANATION_SIGNALS}"
    )]
    TooManyExplanationSignals(usize),
    #[error("AI observation explanation signal {0} has no stable code")]
    MissingExplanationCode(usize),
    #[error("AI observation explanation signal {0} has a non-finite value")]
    NonFiniteExplanationValue(usize),
    #[error(transparent)]
    InvalidProvenance(#[from] RuntimeContractError),
}

#[cfg(test)]
mod tests;
