//! Bounded, rebuildable inventory of usable references, not permission to train a model.

use std::collections::BTreeSet;

use serde::Serialize;
use shadow_domain::{PhotoId, RecipeCommitId};

use super::{
    EditExampleIntent, EditExampleOrigin, FeedbackAction, FeedbackEvent, FeedbackIgnored,
    IncrementalTrainingPolicy, LearningScope, PairwiseExampleRef,
    build_incremental_preference_batch,
};

pub const MAX_LEARNING_READINESS_EVENTS: usize = 512;

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ApprovedEditExampleRef {
    pub event_id: String,
    pub sequence: u64,
    pub photo_id: PhotoId,
    pub baseline_recipe: RecipeCommitId,
    pub approved_recipe: RecipeCommitId,
    pub origin: EditExampleOrigin,
    pub intent: EditExampleIntent,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct LearningReadinessReport {
    pub contract: &'static str,
    pub scope: LearningScope,
    pub events_examined: usize,
    /// References only: exact feature bytes still require source binding and verification.
    pub pairwise_references: Vec<PairwiseExampleRef>,
    /// References only: the application must resolve and validate both immutable Recipes.
    pub approved_edit_references: Vec<ApprovedEditExampleRef>,
    pub excluded: Vec<LearningEvidenceExcluded>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct LearningEvidenceExcluded {
    pub event_id: String,
    pub reason: String,
}

#[derive(Debug, thiserror::Error)]
pub enum LearningReadinessError {
    #[error("learning readiness accepts at most 512 events per page")]
    PageTooLarge,
    #[error("learning evidence has invalid structure or duplicate event identity/sequence")]
    InvalidEvidence,
}

/// Inventories one bounded page of validated human evidence in sequence order.
///
/// No edit history, star, export, or missing click is synthesized into an approval.
/// Forget facts apply to both selection and editing; a later learner must recheck them
/// and rebuild already-trained heads after revocation. Results from separate pages
/// must not be treated as independent samples without dataset-level deduplication.
///
/// # Errors
///
/// Rejects oversized pages, invalid events and duplicate ids/sequences.
pub fn build_learning_readiness(
    events: &[FeedbackEvent],
    scope: &LearningScope,
    forgotten: &BTreeSet<String>,
) -> Result<LearningReadinessReport, LearningReadinessError> {
    if events.len() > MAX_LEARNING_READINESS_EVENTS {
        return Err(LearningReadinessError::PageTooLarge);
    }
    let mut ids = BTreeSet::new();
    let mut sequences = BTreeSet::new();
    for event in events {
        if event.validate().is_err()
            || !ids.insert(&event.event_id)
            || !sequences.insert(event.sequence)
        {
            return Err(LearningReadinessError::InvalidEvidence);
        }
    }
    let mut ordered: Vec<_> = events.iter().collect();
    ordered.sort_by_key(|event| event.sequence);
    let mut report = LearningReadinessReport {
        contract: "shadow.learning-readiness.v1",
        scope: scope.clone(),
        events_examined: events.len(),
        pairwise_references: Vec::new(),
        approved_edit_references: Vec::new(),
        excluded: Vec::new(),
    };
    for event in ordered {
        let reason = if &event.scope != scope {
            Some("outside_scope")
        } else if forgotten.contains(&event.event_id) {
            Some("forgotten")
        } else {
            match event.action {
                FeedbackAction::EditExampleConfirmed {
                    photo_id,
                    baseline_recipe,
                    approved_recipe,
                    origin,
                    intent,
                } => {
                    report
                        .approved_edit_references
                        .push(ApprovedEditExampleRef {
                            event_id: event.event_id.clone(),
                            sequence: event.sequence,
                            photo_id,
                            baseline_recipe,
                            approved_recipe,
                            origin,
                            intent,
                        });
                    None
                }
                FeedbackAction::PairwiseComparison { .. } => {
                    let batch = build_incremental_preference_batch(
                        std::slice::from_ref(event),
                        &IncrementalTrainingPolicy {
                            scope: scope.clone(),
                            learning_paused: false,
                            after_sequence_exclusive: 0,
                            maximum_examples: 1,
                            forgotten_event_ids: BTreeSet::new(),
                        },
                    );
                    report.pairwise_references.extend(batch.batch.examples);
                    batch.ignored.first().map(|reason| match reason {
                        FeedbackIgnored::MissingFrozenFeature { .. } => "missing_frozen_feature",
                        FeedbackIgnored::FeatureSchemaMismatch { .. } => "feature_schema_mismatch",
                        FeedbackIgnored::NonDecisivePairwise { .. } => "non_decisive_pairwise",
                        _ => "ineligible_pairwise",
                    })
                }
                _ => Some("not_explicit_preference_or_edit_approval"),
            }
        };
        if let Some(reason) = reason {
            report.excluded.push(LearningEvidenceExcluded {
                event_id: event.event_id.clone(),
                reason: reason.into(),
            });
        }
    }
    Ok(report)
}

#[cfg(test)]
mod tests;
