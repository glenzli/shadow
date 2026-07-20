use std::collections::{BTreeMap, BTreeSet};

use serde::{Deserialize, Serialize};
use shadow_domain::{GroupId, PhotoId, RecipeCommitId};

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

/// Append-only human evidence. Absence of an event is never a negative label.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "action")]
pub enum FeedbackAction {
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
    }
    if let Some(model) = &presentation.active_model {
        validate_identifier("model provider id", &model.provider_id)?;
        validate_identifier("model id", &model.model_id)?;
        validate_identifier("model revision", &model.model_revision)?;
        validate_identifier("model sha256", &model.model_sha256)?;
        validate_identifier("model preprocessing version", &model.preprocessing_version)?;
        validate_identifier("model input source hash", &model.input_source_hash)?;
        validate_identifier("model cache key", &model.cache_key)?;
    }

    validate_action(action, &presented_photos)
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
        FeedbackAction::Exported { .. } | FeedbackAction::ReturnedForRework { .. } => {}
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

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct PairwiseExampleRef {
    pub event_id: String,
    pub sequence: u64,
    pub scope: LearningScope,
    pub preferred_photo: PhotoId,
    pub other_photo: PhotoId,
    pub preferred_feature: FeatureSnapshotRef,
    pub other_feature: FeatureSnapshotRef,
    /// Explicit pairwise choices are `1.0`; future weaker evidence must be lower.
    pub weight_micros: u32,
}

impl PairwiseExampleRef {
    pub fn weight(&self) -> f64 {
        f64::from(self.weight_micros) / 1_000_000.0
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct IncrementalTrainingBatch {
    pub scope: LearningScope,
    pub after_sequence_exclusive: u64,
    pub through_sequence_inclusive: u64,
    pub examples: Vec<PairwiseExampleRef>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct IncrementalTrainingPolicy {
    pub scope: LearningScope,
    pub learning_paused: bool,
    pub after_sequence_exclusive: u64,
    pub maximum_examples: usize,
    /// Forgetting is non-destructive: source events remain human history but stop training.
    pub forgotten_event_ids: BTreeSet<String>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "reason")]
pub enum FeedbackIgnored {
    LearningPaused,
    AlreadyConsumed { event_id: String },
    Forgotten { event_id: String },
    OutsideLearningScope { event_id: String },
    NotExplicitPairwise { event_id: String },
    NonDecisivePairwise { event_id: String },
    CandidateWasNotPresented { event_id: String, photo_id: PhotoId },
    MissingFrozenFeature { event_id: String, photo_id: PhotoId },
    FeatureSchemaMismatch { event_id: String },
    BatchLimitReached { event_id: String },
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct BatchBuildReport {
    pub batch: IncrementalTrainingBatch,
    pub ignored: Vec<FeedbackIgnored>,
}

/// Builds deterministic training references from explicit pairwise comparisons only.
///
/// Picks, exports, dwell time, and lack of clicks remain recorded feedback but
/// are deliberately not converted to hidden negative examples here.
pub fn build_incremental_preference_batch(
    events: &[FeedbackEvent],
    policy: &IncrementalTrainingPolicy,
) -> BatchBuildReport {
    let mut ordered: Vec<_> = events.iter().collect();
    ordered.sort_by(|left, right| {
        left.sequence
            .cmp(&right.sequence)
            .then_with(|| left.event_id.cmp(&right.event_id))
    });
    let mut report = BatchBuildReport {
        batch: IncrementalTrainingBatch {
            scope: policy.scope.clone(),
            after_sequence_exclusive: policy.after_sequence_exclusive,
            through_sequence_inclusive: policy.after_sequence_exclusive,
            examples: Vec::new(),
        },
        ignored: Vec::new(),
    };

    if policy.learning_paused {
        report.ignored.push(FeedbackIgnored::LearningPaused);
        return report;
    }

    for event in ordered {
        if event.sequence <= policy.after_sequence_exclusive {
            report.ignored.push(FeedbackIgnored::AlreadyConsumed {
                event_id: event.event_id.clone(),
            });
            continue;
        }
        if report.batch.examples.len() >= policy.maximum_examples {
            report.ignored.push(FeedbackIgnored::BatchLimitReached {
                event_id: event.event_id.clone(),
            });
            break;
        }
        report.batch.through_sequence_inclusive =
            report.batch.through_sequence_inclusive.max(event.sequence);
        if policy.forgotten_event_ids.contains(&event.event_id) {
            report.ignored.push(FeedbackIgnored::Forgotten {
                event_id: event.event_id.clone(),
            });
            continue;
        }
        if event.scope != policy.scope {
            report.ignored.push(FeedbackIgnored::OutsideLearningScope {
                event_id: event.event_id.clone(),
            });
            continue;
        }
        match example_from_event(event) {
            Ok(example) => report.batch.examples.push(example),
            Err(ignored) => report.ignored.push(ignored),
        }
    }
    report
}

fn example_from_event(event: &FeedbackEvent) -> Result<PairwiseExampleRef, FeedbackIgnored> {
    let FeedbackAction::PairwiseComparison {
        left,
        right,
        outcome,
    } = event.action
    else {
        return Err(FeedbackIgnored::NotExplicitPairwise {
            event_id: event.event_id.clone(),
        });
    };
    let (preferred, other) = match outcome {
        PairwiseOutcome::LeftPreferred => (left, right),
        PairwiseOutcome::RightPreferred => (right, left),
        PairwiseOutcome::KeepBoth
        | PairwiseOutcome::KeepNeither
        | PairwiseOutcome::CannotCompare => {
            return Err(FeedbackIgnored::NonDecisivePairwise {
                event_id: event.event_id.clone(),
            });
        }
    };
    let preferred_feature = feature_for(event, preferred)?;
    let other_feature = feature_for(event, other)?;
    if preferred_feature.extractor_id != other_feature.extractor_id
        || preferred_feature.extractor_revision != other_feature.extractor_revision
        || preferred_feature.preprocessing_version != other_feature.preprocessing_version
        || preferred_feature.dimension != other_feature.dimension
    {
        return Err(FeedbackIgnored::FeatureSchemaMismatch {
            event_id: event.event_id.clone(),
        });
    }
    Ok(PairwiseExampleRef {
        event_id: event.event_id.clone(),
        sequence: event.sequence,
        scope: event.scope.clone(),
        preferred_photo: preferred,
        other_photo: other,
        preferred_feature,
        other_feature,
        weight_micros: 1_000_000,
    })
}

fn feature_for(
    event: &FeedbackEvent,
    photo_id: PhotoId,
) -> Result<FeatureSnapshotRef, FeedbackIgnored> {
    let Some(candidate) = event
        .presentation
        .candidates
        .iter()
        .find(|candidate| candidate.photo_id == photo_id)
    else {
        return Err(FeedbackIgnored::CandidateWasNotPresented {
            event_id: event.event_id.clone(),
            photo_id,
        });
    };
    candidate
        .feature
        .clone()
        .ok_or_else(|| FeedbackIgnored::MissingFrozenFeature {
            event_id: event.event_id.clone(),
            photo_id,
        })
}

#[cfg(test)]
mod tests {
    use shadow_domain::EntityId;

    use super::*;

    fn candidate(photo_id: PhotoId, artifact_hash: &str) -> PresentedCandidate {
        PresentedCandidate {
            photo_id,
            position: 0,
            visible_fraction: UnitInterval::ONE,
            inspected_at_one_to_one: false,
            feature: Some(FeatureSnapshotRef {
                extractor_id: "frozen-image-features".into(),
                extractor_revision: "r1".into(),
                preprocessing_version: "preview-v1".into(),
                artifact_hash: artifact_hash.into(),
                dimension: 2,
            }),
        }
    }

    fn event(action: FeedbackAction) -> FeedbackEvent {
        FeedbackEvent {
            event_id: "event-1".into(),
            sequence: 1,
            occurred_at_unix_ms: 123,
            scope: LearningScope::Global,
            presentation: PresentationContext {
                session_id: "review-1".into(),
                group_id: None,
                candidates: vec![],
                active_model: None,
            },
            action,
        }
    }

    fn policy() -> IncrementalTrainingPolicy {
        IncrementalTrainingPolicy {
            scope: LearningScope::Global,
            learning_paused: false,
            after_sequence_exclusive: 0,
            maximum_examples: 100,
            forgotten_event_ids: BTreeSet::new(),
        }
    }

    #[test]
    fn explicit_pairwise_choice_becomes_training_evidence() {
        let left = PhotoId::new_v7();
        let right = PhotoId::new_v7();
        let mut comparison = event(FeedbackAction::PairwiseComparison {
            left,
            right,
            outcome: PairwiseOutcome::RightPreferred,
        });
        comparison.presentation.candidates =
            vec![candidate(left, "left"), candidate(right, "right")];
        let report = build_incremental_preference_batch(&[comparison], &policy());
        assert_eq!(report.batch.examples.len(), 1);
        assert_eq!(report.batch.examples[0].preferred_photo, right);
        assert!((report.batch.examples[0].weight() - 1.0).abs() < f64::EPSILON);
    }

    #[test]
    fn export_is_not_silently_converted_to_a_negative_pair() {
        let exported = event(FeedbackAction::Exported {
            photo_id: PhotoId::new_v7(),
        });
        let report = build_incremental_preference_batch(&[exported], &policy());
        assert!(report.batch.examples.is_empty());
        assert!(matches!(
            report.ignored.as_slice(),
            [FeedbackIgnored::NotExplicitPairwise { .. }]
        ));
    }

    #[test]
    fn forgotten_feedback_stays_out_of_future_batches() {
        let mut forgotten = policy();
        forgotten.forgotten_event_ids.insert("event-1".into());
        let report = build_incremental_preference_batch(
            &[event(FeedbackAction::Exported {
                photo_id: PhotoId::new_v7(),
            })],
            &forgotten,
        );
        assert!(matches!(
            report.ignored.as_slice(),
            [FeedbackIgnored::Forgotten { .. }]
        ));
    }

    #[test]
    fn batch_limit_does_not_advance_past_untrained_evidence() {
        let left = PhotoId::new_v7();
        let right = PhotoId::new_v7();
        let mut first = event(FeedbackAction::PairwiseComparison {
            left,
            right,
            outcome: PairwiseOutcome::LeftPreferred,
        });
        first.presentation.candidates = vec![candidate(left, "left"), candidate(right, "right")];
        let mut second = first.clone();
        second.event_id = "event-2".into();
        second.sequence = 2;
        let mut limited = policy();
        limited.maximum_examples = 1;

        let report = build_incremental_preference_batch(&[second, first], &limited);
        assert_eq!(report.batch.examples.len(), 1);
        assert_eq!(report.batch.through_sequence_inclusive, 1);
        assert!(matches!(
            report.ignored.as_slice(),
            [FeedbackIgnored::BatchLimitReached { event_id }] if event_id == "event-2"
        ));
    }

    #[test]
    fn project_feedback_does_not_leak_into_the_global_head() {
        let mut project_event = event(FeedbackAction::Exported {
            photo_id: PhotoId::new_v7(),
        });
        project_event.scope = LearningScope::Project {
            project_id: "wedding-1".into(),
        };
        let report = build_incremental_preference_batch(&[project_event], &policy());
        assert!(report.batch.examples.is_empty());
        assert!(matches!(
            report.ignored.as_slice(),
            [FeedbackIgnored::OutsideLearningScope { .. }]
        ));
    }
}
