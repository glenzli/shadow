use std::collections::BTreeSet;

use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::{ProposalReviewLevel, UnitInterval};

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct ScoredSignal {
    pub value: UnitInterval,
    pub confidence: UnitInterval,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct PersonalPreferenceSignal {
    pub score: UnitInterval,
    pub confidence: UnitInterval,
    pub explicit_evidence_count: u32,
}

/// Explainable selection inputs. These remain separate in storage and UI.
#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct CandidateSignals {
    /// Probability/risk of a severe defect, not a quality score.
    pub hard_defect: ScoredSignal,
    pub technical_quality: ScoredSignal,
    pub general_prior: ScoredSignal,
    pub personal_preference: Option<PersonalPreferenceSignal>,
    pub uniqueness: ScoredSignal,
    /// Similarity to a better candidate, when a grouping provider established one.
    pub duplicate_similarity: Option<ScoredSignal>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CandidateAssessment {
    pub candidate_id: String,
    pub signals: CandidateSignals,
    /// A Pick, star rating, delivered version, or explicit keep always protects visibility.
    pub manually_protected: bool,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct SelectionWeights {
    pub technical_quality: f64,
    pub general_prior: f64,
    pub personal_preference: f64,
    pub uniqueness: f64,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct SelectionPolicy {
    pub weights: SelectionWeights,
    pub hard_defect_threshold: UnitInterval,
    pub hard_defect_minimum_confidence: UnitInterval,
    pub unique_moment_threshold: UnitInterval,
    pub duplicate_collapse_threshold: UnitInterval,
    pub minimum_personal_evidence: u32,
    pub confirmation_threshold: UnitInterval,
    pub proposal_threshold: UnitInterval,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DefectGate {
    Clear,
    ManualReview,
    ExcludedFromRecommendation,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum VisibilityDisposition {
    AlwaysVisible,
    MayCollapseAsDuplicate,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ScoreContribution {
    pub signal: String,
    pub raw_value: UnitInterval,
    pub effective_weight: f64,
    pub weighted_value: f64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CandidateRank {
    pub candidate_id: String,
    pub defect_gate: DefectGate,
    pub visibility: VisibilityDisposition,
    pub composite_score: UnitInterval,
    pub confidence: UnitInterval,
    pub review_level: ProposalReviewLevel,
    pub contributions: Vec<ScoreContribution>,
}

/// Applies the hard-defect gate, then ranks candidates within one comparison group.
///
/// There is intentionally no API here that deletes an asset or writes a Reject.
/// The caller may only render recommendations, review queues, and duplicate folds.
///
/// # Errors
///
/// Returns [`SelectionError`] for duplicate/empty IDs, invalid weights, or inverted thresholds.
pub fn rank_group(
    candidates: &[CandidateAssessment],
    policy: SelectionPolicy,
) -> Result<Vec<CandidateRank>, SelectionError> {
    validate_policy(policy)?;
    let mut ids = BTreeSet::new();
    let mut ranked = Vec::with_capacity(candidates.len());
    for candidate in candidates {
        if candidate.candidate_id.trim().is_empty() {
            return Err(SelectionError::EmptyCandidateId);
        }
        if !ids.insert(candidate.candidate_id.as_str()) {
            return Err(SelectionError::DuplicateCandidateId(
                candidate.candidate_id.clone(),
            ));
        }
        ranked.push(rank_candidate(candidate, policy));
    }
    ranked.sort_by(|left, right| {
        gate_order(left.defect_gate)
            .cmp(&gate_order(right.defect_gate))
            .then_with(|| {
                right
                    .composite_score
                    .get()
                    .total_cmp(&left.composite_score.get())
            })
            .then_with(|| left.candidate_id.cmp(&right.candidate_id))
    });
    Ok(ranked)
}

fn rank_candidate(candidate: &CandidateAssessment, policy: SelectionPolicy) -> CandidateRank {
    let gate = defect_gate(candidate, policy);
    let personal_weight = candidate
        .signals
        .personal_preference
        .map_or(0.0, |personal| {
            let evidence_ramp = if policy.minimum_personal_evidence == 0 {
                1.0
            } else {
                (f64::from(personal.explicit_evidence_count)
                    / f64::from(policy.minimum_personal_evidence))
                .min(1.0)
            };
            policy.weights.personal_preference * personal.confidence.get() * evidence_ramp
        });
    let personal_score = candidate
        .signals
        .personal_preference
        .map_or(UnitInterval::ZERO, |personal| personal.score);
    let parts = [
        (
            "technical_quality",
            candidate.signals.technical_quality,
            policy.weights.technical_quality,
        ),
        (
            "general_prior",
            candidate.signals.general_prior,
            policy.weights.general_prior,
        ),
        (
            "personal_preference",
            ScoredSignal {
                value: personal_score,
                confidence: candidate
                    .signals
                    .personal_preference
                    .map_or(UnitInterval::ZERO, |personal| personal.confidence),
            },
            personal_weight,
        ),
        (
            "uniqueness",
            candidate.signals.uniqueness,
            policy.weights.uniqueness,
        ),
    ];
    let score_parts: Vec<_> = parts
        .iter()
        .map(|(_, signal, weight)| (signal.value, *weight))
        .collect();
    let confidence_parts: Vec<_> = parts
        .iter()
        .map(|(_, signal, weight)| (signal.confidence, *weight))
        .collect();
    let composite_score = UnitInterval::weighted_average(&score_parts);
    let confidence = UnitInterval::weighted_average(&confidence_parts);
    let mut review_level = ProposalReviewLevel::from_confidence(
        confidence,
        policy.confirmation_threshold,
        policy.proposal_threshold,
    );
    if gate == DefectGate::ManualReview && review_level == ProposalReviewLevel::ProposalOnly {
        review_level = ProposalReviewLevel::NeedsConfirmation;
    } else if gate == DefectGate::ExcludedFromRecommendation {
        review_level = ProposalReviewLevel::ObservationOnly;
    }
    let visibility = visibility(candidate, policy);
    let contributions = parts
        .into_iter()
        .map(|(name, signal, weight)| ScoreContribution {
            signal: name.into(),
            raw_value: signal.value,
            effective_weight: weight,
            weighted_value: signal.value.get() * weight,
        })
        .collect();

    CandidateRank {
        candidate_id: candidate.candidate_id.clone(),
        defect_gate: gate,
        visibility,
        composite_score,
        confidence,
        review_level,
        contributions,
    }
}

fn defect_gate(candidate: &CandidateAssessment, policy: SelectionPolicy) -> DefectGate {
    let hard = candidate.signals.hard_defect;
    if hard.value < policy.hard_defect_threshold {
        return DefectGate::Clear;
    }
    if candidate.manually_protected
        || candidate.signals.uniqueness.value >= policy.unique_moment_threshold
        || hard.confidence < policy.hard_defect_minimum_confidence
    {
        DefectGate::ManualReview
    } else {
        DefectGate::ExcludedFromRecommendation
    }
}

fn visibility(candidate: &CandidateAssessment, policy: SelectionPolicy) -> VisibilityDisposition {
    if candidate.manually_protected
        || candidate.signals.uniqueness.value >= policy.unique_moment_threshold
    {
        return VisibilityDisposition::AlwaysVisible;
    }
    match candidate.signals.duplicate_similarity {
        Some(similarity)
            if similarity.value >= policy.duplicate_collapse_threshold
                && similarity.confidence >= policy.confirmation_threshold =>
        {
            VisibilityDisposition::MayCollapseAsDuplicate
        }
        _ => VisibilityDisposition::AlwaysVisible,
    }
}

const fn gate_order(gate: DefectGate) -> u8 {
    match gate {
        DefectGate::Clear => 0,
        DefectGate::ManualReview => 1,
        DefectGate::ExcludedFromRecommendation => 2,
    }
}

fn validate_policy(policy: SelectionPolicy) -> Result<(), SelectionError> {
    let weights = [
        policy.weights.technical_quality,
        policy.weights.general_prior,
        policy.weights.personal_preference,
        policy.weights.uniqueness,
    ];
    if weights
        .iter()
        .any(|weight| !weight.is_finite() || *weight < 0.0)
        || weights.iter().all(|weight| *weight == 0.0)
    {
        return Err(SelectionError::InvalidWeights);
    }
    if policy.confirmation_threshold > policy.proposal_threshold {
        return Err(SelectionError::InvertedConfidenceThresholds);
    }
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum SelectionError {
    #[error("candidate id must not be empty")]
    EmptyCandidateId,
    #[error("candidate id appears twice in one group: {0}")]
    DuplicateCandidateId(String),
    #[error("selection weights must be finite, non-negative, and not all zero")]
    InvalidWeights,
    #[error("confirmation threshold must not exceed proposal threshold")]
    InvertedConfidenceThresholds,
}

#[cfg(test)]
mod tests;
