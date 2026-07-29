use std::collections::{BTreeMap, BTreeSet};

use serde::{Deserialize, Serialize};
use shadow_domain::PhotoId;
use thiserror::Error;

pub const MAX_FEATURE_PRINT_CANDIDATES: usize = 512;

/// Finite, non-negative distance supplied by one feature-print provider.
///
/// The value is deliberately not normalized to `[0, 1]`; Apple Vision only
/// defines shorter distance as more similar, so thresholds require
/// Shadow-owned calibration for one pinned request revision.
#[derive(Debug, Copy, Clone, PartialEq, PartialOrd, Serialize, Deserialize)]
#[serde(transparent)]
pub struct FeaturePrintDistanceValue(f32);

impl FeaturePrintDistanceValue {
    /// Creates a provider distance without inventing a normalized confidence.
    ///
    /// # Errors
    ///
    /// Returns an error for negative or non-finite values.
    pub fn new(value: f32) -> Result<Self, SimilarityEvidenceError> {
        if value.is_finite() && value >= 0.0 {
            Ok(Self(value))
        } else {
            Err(SimilarityEvidenceError::InvalidDistance)
        }
    }

    pub const fn get(self) -> f32 {
        self.0
    }

    fn validate(self) -> Result<(), SimilarityEvidenceError> {
        Self::new(self.0).map(|_| ())
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct FeaturePrintCandidate {
    pub photo_id: PhotoId,
    pub capture_order: u32,
    pub feature_print_digest: String,
    /// Protected candidates remain visible and are never converted into a
    /// hidden/rejected disposition by this module.
    pub manually_protected: bool,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct FeaturePrintDistance {
    pub left: PhotoId,
    pub right: PhotoId,
    pub distance: FeaturePrintDistanceValue,
}

/// Complete pairwise evidence for one caller-selected capture-time window.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FeaturePrintDistanceBatch {
    pub comparison_revision: String,
    pub candidates: Vec<FeaturePrintCandidate>,
    pub distances: Vec<FeaturePrintDistance>,
}

impl FeaturePrintDistanceBatch {
    /// Validates one exact and complete pairwise comparison matrix.
    ///
    /// # Errors
    ///
    /// Returns an error for missing/duplicate candidates, malformed feature
    /// identities, invalid distances, or an incomplete pair matrix.
    pub fn validate(&self) -> Result<(), SimilarityEvidenceError> {
        if self.comparison_revision.trim().is_empty() {
            return Err(SimilarityEvidenceError::MissingComparisonRevision);
        }
        if !(2..=MAX_FEATURE_PRINT_CANDIDATES).contains(&self.candidates.len()) {
            return Err(SimilarityEvidenceError::InvalidCandidateCount(
                self.candidates.len(),
            ));
        }

        let mut candidates = BTreeSet::new();
        for candidate in &self.candidates {
            if !candidates.insert(candidate.photo_id) {
                return Err(SimilarityEvidenceError::DuplicateCandidate(
                    candidate.photo_id,
                ));
            }
            validate_digest(&candidate.feature_print_digest)?;
        }

        let expected_pairs = self
            .candidates
            .len()
            .checked_mul(self.candidates.len().saturating_sub(1))
            .and_then(|pairs| pairs.checked_div(2))
            .ok_or(SimilarityEvidenceError::InvalidCandidateCount(
                self.candidates.len(),
            ))?;
        if self.distances.len() != expected_pairs {
            return Err(SimilarityEvidenceError::IncompleteDistanceMatrix {
                expected: expected_pairs,
                actual: self.distances.len(),
            });
        }
        let mut pairs = BTreeSet::new();
        for evidence in &self.distances {
            evidence.distance.validate()?;
            if evidence.left == evidence.right {
                return Err(SimilarityEvidenceError::SelfDistance(evidence.left));
            }
            if !candidates.contains(&evidence.left) {
                return Err(SimilarityEvidenceError::UnknownCandidate(evidence.left));
            }
            if !candidates.contains(&evidence.right) {
                return Err(SimilarityEvidenceError::UnknownCandidate(evidence.right));
            }
            let pair = ordered_pair(evidence.left, evidence.right);
            if !pairs.insert(pair) {
                return Err(SimilarityEvidenceError::DuplicateDistancePair {
                    left: pair.0,
                    right: pair.1,
                });
            }
        }
        Ok(())
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct SimilarityGroupingPolicy {
    pub maximum_pair_distance: FeaturePrintDistanceValue,
    pub minimum_group_size: u16,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RepresentativeSuggestionBasis {
    /// Candidate with the smallest total distance to the other group members.
    ///
    /// This is a similarity medoid and says nothing about focus, expressions,
    /// aesthetics, or photographic quality.
    SimilarityMedoid,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct SimilarityReviewStart {
    pub photo_id: PhotoId,
    pub basis: RepresentativeSuggestionBasis,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct SimilarityReviewGroup {
    pub members: Vec<PhotoId>,
    /// Suggested first item to inspect, never an automatic Pick.
    pub review_start: SimilarityReviewStart,
}

/// Lossless review-compression proposal. Every input remains in exactly one
/// group or in `ungrouped`; the output has no Pick/Reject mutation type.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct SimilarityReviewPlan {
    pub groups: Vec<SimilarityReviewGroup>,
    pub ungrouped: Vec<PhotoId>,
}

/// Builds deterministic complete-link groups and a similarity medoid review
/// start for each group.
///
/// `FeaturePrint` is similarity evidence only. Technical quality, face capture
/// quality, aesthetics, and personal preference remain separate signals.
///
/// # Errors
///
/// Returns an error for malformed evidence or a grouping policy that cannot
/// form a multi-photo group.
pub fn propose_similarity_review(
    batch: &FeaturePrintDistanceBatch,
    policy: SimilarityGroupingPolicy,
) -> Result<SimilarityReviewPlan, SimilarityEvidenceError> {
    batch.validate()?;
    policy.maximum_pair_distance.validate()?;
    if policy.minimum_group_size < 2
        || usize::from(policy.minimum_group_size) > batch.candidates.len()
    {
        return Err(SimilarityEvidenceError::InvalidMinimumGroupSize(
            policy.minimum_group_size,
        ));
    }

    let by_photo = batch
        .candidates
        .iter()
        .map(|candidate| (candidate.photo_id, candidate))
        .collect::<BTreeMap<_, _>>();
    let distances = batch
        .distances
        .iter()
        .map(|evidence| {
            (
                ordered_pair(evidence.left, evidence.right),
                evidence.distance,
            )
        })
        .collect::<BTreeMap<_, _>>();
    let mut ordered = batch.candidates.iter().collect::<Vec<_>>();
    ordered.sort_by_key(|candidate| (candidate.capture_order, candidate.photo_id));

    let mut clusters: Vec<Vec<PhotoId>> = Vec::new();
    for candidate in ordered {
        let compatible = clusters.iter().position(|cluster| {
            cluster.iter().all(|member| {
                distances[&ordered_pair(*member, candidate.photo_id)]
                    <= policy.maximum_pair_distance
            })
        });
        if let Some(index) = compatible {
            clusters[index].push(candidate.photo_id);
        } else {
            clusters.push(vec![candidate.photo_id]);
        }
    }

    let mut groups = Vec::new();
    let mut ungrouped = Vec::new();
    for mut cluster in clusters {
        cluster.sort_by_key(|photo_id| (by_photo[photo_id].capture_order, *photo_id));
        if cluster.len() < usize::from(policy.minimum_group_size) {
            ungrouped.extend(cluster);
            continue;
        }
        let review_start = similarity_medoid(&cluster, &distances, &by_photo);
        groups.push(SimilarityReviewGroup {
            members: cluster,
            review_start: SimilarityReviewStart {
                photo_id: review_start,
                basis: RepresentativeSuggestionBasis::SimilarityMedoid,
            },
        });
    }
    ungrouped.sort_by_key(|photo_id| (by_photo[photo_id].capture_order, *photo_id));

    Ok(SimilarityReviewPlan { groups, ungrouped })
}

fn similarity_medoid(
    cluster: &[PhotoId],
    distances: &BTreeMap<(PhotoId, PhotoId), FeaturePrintDistanceValue>,
    candidates: &BTreeMap<PhotoId, &FeaturePrintCandidate>,
) -> PhotoId {
    cluster
        .iter()
        .copied()
        .min_by(|left, right| {
            let left_total = total_distance(*left, cluster, distances);
            let right_total = total_distance(*right, cluster, distances);
            left_total
                .total_cmp(&right_total)
                .then_with(|| {
                    candidates[left]
                        .capture_order
                        .cmp(&candidates[right].capture_order)
                })
                .then_with(|| left.cmp(right))
        })
        .expect("validated groups are non-empty")
}

fn total_distance(
    candidate: PhotoId,
    cluster: &[PhotoId],
    distances: &BTreeMap<(PhotoId, PhotoId), FeaturePrintDistanceValue>,
) -> f64 {
    cluster
        .iter()
        .filter(|other| **other != candidate)
        .map(|other| f64::from(distances[&ordered_pair(candidate, *other)].get()))
        .sum()
}

fn ordered_pair(left: PhotoId, right: PhotoId) -> (PhotoId, PhotoId) {
    if left < right {
        (left, right)
    } else {
        (right, left)
    }
}

fn validate_digest(value: &str) -> Result<(), SimilarityEvidenceError> {
    if value.len() == 64 && value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        Ok(())
    } else {
        Err(SimilarityEvidenceError::InvalidFeaturePrintDigest)
    }
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum SimilarityEvidenceError {
    #[error("feature-print comparison revision must not be empty")]
    MissingComparisonRevision,
    #[error("feature-print candidate count must be in 2..={MAX_FEATURE_PRINT_CANDIDATES}, got {0}")]
    InvalidCandidateCount(usize),
    #[error("feature-print candidate {0} appears more than once")]
    DuplicateCandidate(PhotoId),
    #[error("feature-print digest must be a 256-bit hexadecimal digest")]
    InvalidFeaturePrintDigest,
    #[error("feature-print distance must be finite and non-negative")]
    InvalidDistance,
    #[error("distance matrix expected {expected} pairs but received {actual}")]
    IncompleteDistanceMatrix { expected: usize, actual: usize },
    #[error("feature-print distance refers to unknown candidate {0}")]
    UnknownCandidate(PhotoId),
    #[error("feature-print distance cannot compare candidate {0} with itself")]
    SelfDistance(PhotoId),
    #[error("feature-print pair {left}/{right} appears more than once")]
    DuplicateDistancePair { left: PhotoId, right: PhotoId },
    #[error("minimum similarity group size must be in 2..=candidate_count, got {0}")]
    InvalidMinimumGroupSize(u16),
}

#[cfg(test)]
mod tests;
