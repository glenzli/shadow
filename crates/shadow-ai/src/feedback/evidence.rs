//! Append-only human evidence, presentation provenance, and structural validation.

use std::collections::{BTreeMap, BTreeSet};

use serde::{Deserialize, Serialize};
use shadow_domain::{GroupId, PhotoId, RecipeCommitId, RepresentationId};

use crate::{ModelProvenance, UnitInterval};

const MAX_IDENTIFIER_LENGTH: usize = 256;
const MAX_TEXT_LENGTH: usize = 4_096;
const MAX_PRESENTED_CANDIDATES: usize = 4_096;
const MAX_TARGET_PHOTOS: usize = 4_096;

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum LearningScope {
    Global,
    Project { project_id: String },
}

/// Identity of a frozen feature artifact. Vectors themselves remain rebuildable data.
#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
pub struct FeatureSnapshotRef {
    pub extractor_id: String,
    pub extractor_revision: String,
    pub preprocessing_version: String,
    pub artifact_hash: String,
    pub dimension: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PresentedCandidate {
    pub photo_id: PhotoId,
    pub position: u32,
    pub visible_fraction: UnitInterval,
    pub inspected_at_one_to_one: bool,
    pub feature: Option<FeatureSnapshotRef>,
    /// Exact rebuildable artifact and decoded frame that reached the UI.
    ///
    /// Older evidence did not capture this boundary. Omitting `None` preserves
    /// its canonical JSON byte-for-byte instead of inventing provenance later.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub visual: Option<PresentedVisualProvenance>,
}

/// Catalog role of the rebuildable artifact shown to the user.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PresentedVisualRole {
    RecipePreview,
    EmbeddedPreview,
    GeneratedProxy,
}

/// Spatial fitting applied by the evidence surface.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PresentedFitMode {
    PreserveAspectFit,
}

/// Frozen identity of the rebuildable Catalog artifact used for presentation.
///
/// This deliberately duplicates source and artifact metadata. Feedback history
/// must remain explainable after the corresponding cache row is invalidated.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct PresentedVisualArtifact {
    pub representation_id: RepresentationId,
    pub source_byte_len: u64,
    pub source_modified_at_ms: Option<i64>,
    pub role: PresentedVisualRole,
    pub variant_key: String,
    pub generator_id: String,
    pub generator_version: String,
    pub provider_preview_id: Option<u64>,
    pub blob_algorithm: String,
    pub blob_digest_hex: String,
    pub blob_byte_len: u64,
    pub codec: String,
    pub byte_order: String,
    pub width: u32,
    pub height: u32,
    pub bits_per_channel: u16,
    pub channels: u16,
    pub created_at_ms: i64,
}

/// Exact decoded frame and UI surface contract used for one presentation.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct PresentedVisualFrame {
    pub surface_id: String,
    pub surface_revision: u64,
    pub fit_mode: PresentedFitMode,
    pub decoder_id: String,
    pub decoder_version: String,
    pub auto_transform: bool,
    pub requested_width: u32,
    pub requested_height: u32,
    pub decoded_width: u32,
    pub decoded_height: u32,
    pub pixel_format: String,
    pub pixel_hash_algorithm: String,
    pub pixel_hash_hex: String,
}

/// Complete provenance for the pixels a candidate surface presented.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct PresentedVisualProvenance {
    pub artifact: PresentedVisualArtifact,
    pub frame: PresentedVisualFrame,
}

/// What the user could actually see when an action occurred.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PresentationContext {
    pub session_id: String,
    pub group_id: Option<GroupId>,
    pub candidates: Vec<PresentedCandidate>,
    pub active_model: Option<ModelProvenance>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PairwiseOutcome {
    LeftPreferred,
    RightPreferred,
    KeepBoth,
    KeepNeither,
    CannotCompare,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SuggestionDecision {
    Accepted,
    Rejected,
    AcceptedThenUndone,
}

/// User-declared provenance; approval does not turn imported or assisted edits into manual work.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum EditExampleOrigin {
    Manual,
    Imported,
    Assisted,
    Mixed,
    Unknown,
}

/// Technical corrections and creative style must not share an undifferentiated target.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum EditExampleIntent {
    PersonalStyle,
    TechnicalCorrection,
}

/// Append-only human evidence. Absence of an event is never a negative label.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "action")]
pub enum FeedbackAction {
    /// Explicit approval of exact immutable states, never inferred from autosave or export.
    /// Equal states are valid evidence that the user wanted no adjustment.
    EditExampleConfirmed {
        photo_id: PhotoId,
        baseline_recipe: RecipeCommitId,
        approved_recipe: RecipeCommitId,
        origin: EditExampleOrigin,
        intent: EditExampleIntent,
    },
    PairwiseComparison {
        left: PhotoId,
        right: PhotoId,
        outcome: PairwiseOutcome,
    },
    FlagChanged {
        photo_id: PhotoId,
        before: Option<String>,
        after: Option<String>,
    },
    RatingChanged {
        photo_id: PhotoId,
        before: Option<u8>,
        after: Option<u8>,
    },
    SuggestionReviewed {
        proposal_id: String,
        decision: SuggestionDecision,
    },
    DevelopProposalEdited {
        proposal_id: String,
        suggested_recipe: RecipeCommitId,
        final_recipe: RecipeCommitId,
        /// Semantic parameter names and normalized final-minus-suggested residuals.
        parameter_residuals: BTreeMap<String, f64>,
    },
    ParametersCopied {
        source_photo: PhotoId,
        target_photos: Vec<PhotoId>,
    },
    Exported {
        photo_id: PhotoId,
    },
    ReturnedForRework {
        photo_id: PhotoId,
    },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FeedbackEvent {
    pub event_id: String,
    /// Catalog-assigned monotonic sequence; wall-clock time is not ordering truth.
    pub sequence: u64,
    pub occurred_at_unix_ms: i64,
    pub scope: LearningScope,
    pub presentation: PresentationContext,
    pub action: FeedbackAction,
}

/// Feedback waiting for the Catalog to assign its authoritative sequence.
///
/// Application code must never guess the next sequence. The Catalog validates
/// this value, allocates a monotonic sequence, and returns a [`FeedbackEvent`].
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct NewFeedbackEvent {
    pub event_id: String,
    pub occurred_at_unix_ms: i64,
    pub scope: LearningScope,
    pub presentation: PresentationContext,
    pub action: FeedbackAction,
}

/// An append-only request to stop using one source event as training evidence.
/// The referenced [`FeedbackEvent`] remains intact as human history.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct NewFeedbackForgetFact {
    pub fact_id: String,
    pub target_event_id: String,
    pub occurred_at_unix_ms: i64,
    pub reason: Option<String>,
}

/// A durable forget fact with its Catalog-assigned sequence.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct FeedbackForgetFact {
    pub fact_id: String,
    pub sequence: u64,
    pub target_event_id: String,
    pub occurred_at_unix_ms: i64,
    pub reason: Option<String>,
}

#[derive(Debug, Clone, Eq, PartialEq, thiserror::Error)]
pub enum FeedbackValidationError {
    #[error("{field} must not be empty")]
    EmptyString { field: &'static str },
    #[error("{field} exceeds its maximum length of {maximum}")]
    StringTooLong { field: &'static str, maximum: usize },
    #[error("feedback sequence must be greater than zero")]
    ZeroSequence,
    #[error("presentation contains more than {maximum} candidates")]
    TooManyCandidates { maximum: usize },
    #[error("photo {0} appears more than once in the presentation")]
    DuplicatePresentedPhoto(PhotoId),
    #[error("presentation position {0} appears more than once")]
    DuplicatePresentationPosition(u32),
    #[error("pairwise comparison must reference two different photos")]
    PairwisePhotosAreEqual,
    #[error("pairwise photo {0} was not present in the presentation")]
    PairwisePhotoNotPresented(PhotoId),
    #[error("rating {0} is outside the supported 0 through 5 range")]
    RatingOutOfRange(u8),
    #[error("parameter residual {parameter:?} must be finite")]
    NonFiniteParameterResidual { parameter: String },
    #[error("parameters-copied feedback must contain at least one target")]
    MissingCopyTarget,
    #[error("parameters-copied feedback contains more than {maximum} targets")]
    TooManyCopyTargets { maximum: usize },
    #[error("photo {0} appears more than once in parameters-copied targets")]
    DuplicateCopyTarget(PhotoId),
    #[error("parameters cannot be copied from a photo to itself")]
    CopyTargetIsSource,
    #[error("feature dimension must be greater than zero")]
    ZeroFeatureDimension,
    #[error("{field} must be greater than zero")]
    ZeroVisualValue { field: &'static str },
    #[error("{field} must be exactly 64 lowercase hexadecimal characters")]
    InvalidVisualDigest { field: &'static str },
    #[error("active model execution route is incomplete or internally inconsistent")]
    InvalidModelRoute,
    #[error("{field} must be exactly 64 lowercase hexadecimal characters")]
    InvalidModelDigest { field: &'static str },
}

impl NewFeedbackEvent {
    /// Validates structure that is independent of Catalog contents.
    ///
    /// # Errors
    ///
    /// Returns [`FeedbackValidationError`] for malformed or ambiguous evidence.
    pub fn validate(&self) -> Result<(), FeedbackValidationError> {
        validate_feedback(
            &self.event_id,
            &self.scope,
            &self.presentation,
            &self.action,
        )
    }

    /// Adds a Catalog-assigned sequence after validating the event.
    ///
    /// # Errors
    ///
    /// Returns [`FeedbackValidationError`] when the event is invalid or the
    /// supplied sequence is zero.
    pub fn with_sequence(self, sequence: u64) -> Result<FeedbackEvent, FeedbackValidationError> {
        if sequence == 0 {
            return Err(FeedbackValidationError::ZeroSequence);
        }
        self.validate()?;
        Ok(FeedbackEvent {
            event_id: self.event_id,
            sequence,
            occurred_at_unix_ms: self.occurred_at_unix_ms,
            scope: self.scope,
            presentation: self.presentation,
            action: self.action,
        })
    }
}

impl FeedbackEvent {
    /// Validates both the Catalog sequence and the human-evidence payload.
    ///
    /// # Errors
    ///
    /// Returns [`FeedbackValidationError`] for invalid persisted evidence.
    pub fn validate(&self) -> Result<(), FeedbackValidationError> {
        if self.sequence == 0 {
            return Err(FeedbackValidationError::ZeroSequence);
        }
        validate_feedback(
            &self.event_id,
            &self.scope,
            &self.presentation,
            &self.action,
        )
    }
}

impl NewFeedbackForgetFact {
    /// Validates an unsequenced forget fact.
    ///
    /// # Errors
    ///
    /// Returns [`FeedbackValidationError`] for empty or oversized text.
    pub fn validate(&self) -> Result<(), FeedbackValidationError> {
        validate_identifier("forget fact id", &self.fact_id)?;
        validate_identifier("target feedback event id", &self.target_event_id)?;
        if let Some(reason) = &self.reason {
            validate_optional_text("forget reason", reason)?;
        }
        Ok(())
    }

    /// Adds a Catalog-assigned forget sequence.
    ///
    /// # Errors
    ///
    /// Returns [`FeedbackValidationError`] when the fact is invalid or the
    /// supplied sequence is zero.
    pub fn with_sequence(
        self,
        sequence: u64,
    ) -> Result<FeedbackForgetFact, FeedbackValidationError> {
        if sequence == 0 {
            return Err(FeedbackValidationError::ZeroSequence);
        }
        self.validate()?;
        Ok(FeedbackForgetFact {
            fact_id: self.fact_id,
            sequence,
            target_event_id: self.target_event_id,
            occurred_at_unix_ms: self.occurred_at_unix_ms,
            reason: self.reason,
        })
    }
}

impl FeedbackForgetFact {
    /// Validates a sequenced forget fact.
    ///
    /// # Errors
    ///
    /// Returns [`FeedbackValidationError`] for malformed persisted data.
    pub fn validate(&self) -> Result<(), FeedbackValidationError> {
        if self.sequence == 0 {
            return Err(FeedbackValidationError::ZeroSequence);
        }
        NewFeedbackForgetFact {
            fact_id: self.fact_id.clone(),
            target_event_id: self.target_event_id.clone(),
            occurred_at_unix_ms: self.occurred_at_unix_ms,
            reason: self.reason.clone(),
        }
        .validate()
    }
}

fn validate_feedback(
    event_id: &str,
    scope: &LearningScope,
    presentation: &PresentationContext,
    action: &FeedbackAction,
) -> Result<(), FeedbackValidationError> {
    validate_identifier("feedback event id", event_id)?;
    if let LearningScope::Project { project_id } = scope {
        validate_identifier("project id", project_id)?;
    }
    validate_identifier("presentation session id", &presentation.session_id)?;
    if presentation.candidates.len() > MAX_PRESENTED_CANDIDATES {
        return Err(FeedbackValidationError::TooManyCandidates {
            maximum: MAX_PRESENTED_CANDIDATES,
        });
    }

    let mut presented_photos = BTreeSet::new();
    let mut positions = BTreeSet::new();
    for candidate in &presentation.candidates {
        if !presented_photos.insert(candidate.photo_id) {
            return Err(FeedbackValidationError::DuplicatePresentedPhoto(
                candidate.photo_id,
            ));
        }
        if !positions.insert(candidate.position) {
            return Err(FeedbackValidationError::DuplicatePresentationPosition(
                candidate.position,
            ));
        }
        if let Some(feature) = &candidate.feature {
            validate_identifier("feature extractor id", &feature.extractor_id)?;
            validate_identifier("feature extractor revision", &feature.extractor_revision)?;
            validate_identifier(
                "feature preprocessing version",
                &feature.preprocessing_version,
            )?;
            validate_identifier("feature artifact hash", &feature.artifact_hash)?;
            if feature.dimension == 0 {
                return Err(FeedbackValidationError::ZeroFeatureDimension);
            }
        }
        if let Some(visual) = &candidate.visual {
            validate_presented_visual(visual)?;
        }
    }
    if let Some(model) = &presentation.active_model {
        model
            .validate()
            .map_err(|_| FeedbackValidationError::InvalidModelRoute)?;
    }

    validate_action(action, &presented_photos)
}

fn validate_presented_visual(
    visual: &PresentedVisualProvenance,
) -> Result<(), FeedbackValidationError> {
    let artifact = &visual.artifact;
    for (field, value) in [
        ("visual variant key", artifact.variant_key.as_str()),
        ("visual generator id", artifact.generator_id.as_str()),
        (
            "visual generator version",
            artifact.generator_version.as_str(),
        ),
        ("visual blob algorithm", artifact.blob_algorithm.as_str()),
        ("visual codec", artifact.codec.as_str()),
        ("visual byte order", artifact.byte_order.as_str()),
    ] {
        validate_identifier(field, value)?;
    }
    validate_lower_hex_digest("visual blob digest", &artifact.blob_digest_hex)?;
    for (field, value) in [
        ("visual source byte length", artifact.source_byte_len),
        ("visual blob byte length", artifact.blob_byte_len),
        ("visual artifact width", u64::from(artifact.width)),
        ("visual artifact height", u64::from(artifact.height)),
        (
            "visual artifact bits per channel",
            u64::from(artifact.bits_per_channel),
        ),
        ("visual artifact channels", u64::from(artifact.channels)),
    ] {
        validate_non_zero_visual_value(field, value)?;
    }

    let frame = &visual.frame;
    for (field, value) in [
        ("visual surface id", frame.surface_id.as_str()),
        ("visual decoder id", frame.decoder_id.as_str()),
        ("visual decoder version", frame.decoder_version.as_str()),
        ("visual pixel format", frame.pixel_format.as_str()),
        (
            "visual pixel hash algorithm",
            frame.pixel_hash_algorithm.as_str(),
        ),
    ] {
        validate_identifier(field, value)?;
    }
    validate_lower_hex_digest("visual pixel hash", &frame.pixel_hash_hex)?;
    for (field, value) in [
        ("visual surface revision", frame.surface_revision),
        ("visual requested width", u64::from(frame.requested_width)),
        ("visual requested height", u64::from(frame.requested_height)),
        ("visual decoded width", u64::from(frame.decoded_width)),
        ("visual decoded height", u64::from(frame.decoded_height)),
    ] {
        validate_non_zero_visual_value(field, value)?;
    }
    Ok(())
}

fn validate_non_zero_visual_value(
    field: &'static str,
    value: u64,
) -> Result<(), FeedbackValidationError> {
    if value == 0 {
        Err(FeedbackValidationError::ZeroVisualValue { field })
    } else {
        Ok(())
    }
}

fn validate_lower_hex_digest(
    field: &'static str,
    value: &str,
) -> Result<(), FeedbackValidationError> {
    if value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        Ok(())
    } else {
        Err(FeedbackValidationError::InvalidVisualDigest { field })
    }
}

fn validate_action(
    action: &FeedbackAction,
    presented_photos: &BTreeSet<PhotoId>,
) -> Result<(), FeedbackValidationError> {
    match action {
        FeedbackAction::PairwiseComparison { left, right, .. } => {
            if left == right {
                return Err(FeedbackValidationError::PairwisePhotosAreEqual);
            }
            for photo_id in [left, right] {
                if !presented_photos.contains(photo_id) {
                    return Err(FeedbackValidationError::PairwisePhotoNotPresented(
                        *photo_id,
                    ));
                }
            }
        }
        FeedbackAction::FlagChanged { before, after, .. } => {
            for value in [before, after].into_iter().flatten() {
                validate_optional_text("flag value", value)?;
            }
        }
        FeedbackAction::RatingChanged { before, after, .. } => {
            for rating in [before, after].into_iter().flatten() {
                if *rating > 5 {
                    return Err(FeedbackValidationError::RatingOutOfRange(*rating));
                }
            }
        }
        FeedbackAction::SuggestionReviewed { proposal_id, .. } => {
            validate_identifier("proposal id", proposal_id)?;
        }
        FeedbackAction::DevelopProposalEdited {
            proposal_id,
            parameter_residuals,
            ..
        } => {
            validate_identifier("proposal id", proposal_id)?;
            for (parameter, residual) in parameter_residuals {
                validate_identifier("parameter residual name", parameter)?;
                if !residual.is_finite() {
                    return Err(FeedbackValidationError::NonFiniteParameterResidual {
                        parameter: parameter.clone(),
                    });
                }
            }
        }
        FeedbackAction::ParametersCopied {
            source_photo,
            target_photos,
        } => {
            if target_photos.is_empty() {
                return Err(FeedbackValidationError::MissingCopyTarget);
            }
            if target_photos.len() > MAX_TARGET_PHOTOS {
                return Err(FeedbackValidationError::TooManyCopyTargets {
                    maximum: MAX_TARGET_PHOTOS,
                });
            }
            let mut unique = BTreeSet::new();
            for target in target_photos {
                if target == source_photo {
                    return Err(FeedbackValidationError::CopyTargetIsSource);
                }
                if !unique.insert(*target) {
                    return Err(FeedbackValidationError::DuplicateCopyTarget(*target));
                }
            }
        }
        FeedbackAction::EditExampleConfirmed { .. }
        | FeedbackAction::Exported { .. }
        | FeedbackAction::ReturnedForRework { .. } => {}
    }
    Ok(())
}

fn validate_identifier(field: &'static str, value: &str) -> Result<(), FeedbackValidationError> {
    if value.trim().is_empty() {
        return Err(FeedbackValidationError::EmptyString { field });
    }
    if value.len() > MAX_IDENTIFIER_LENGTH {
        return Err(FeedbackValidationError::StringTooLong {
            field,
            maximum: MAX_IDENTIFIER_LENGTH,
        });
    }
    Ok(())
}

fn validate_optional_text(field: &'static str, value: &str) -> Result<(), FeedbackValidationError> {
    if value.trim().is_empty() {
        return Err(FeedbackValidationError::EmptyString { field });
    }
    if value.len() > MAX_TEXT_LENGTH {
        return Err(FeedbackValidationError::StringTooLong {
            field,
            maximum: MAX_TEXT_LENGTH,
        });
    }
    Ok(())
}

#[cfg(test)]
mod tests;
