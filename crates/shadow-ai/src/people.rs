//! Provider-neutral face evidence and conservative anonymous-person grouping.
//!
//! Embeddings remain request-local sensitive evidence. The public grouping plan
//! carries only occurrence references and never serializes biometric vectors.

use std::{collections::BTreeMap, fmt};

use serde::{Deserialize, Serialize};
use shadow_domain::{PhotoId, RepresentationId};
use thiserror::Error;

pub const SFACE_EMBEDDING_DIMENSIONS: usize = 128;
pub const MAX_FACE_OCCURRENCES: usize = 4_096;
pub const ANONYMOUS_PEOPLE_GROUPING_REVISION: &str =
    "shadow.sface-complete-link-no-cooccurrence.20260810.1";

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct FacePoint {
    pub x: f32,
    pub y: f32,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct FaceBoundingBox {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct FaceLandmarks {
    pub right_eye: FacePoint,
    pub left_eye: FacePoint,
    pub nose_tip: FacePoint,
    pub right_mouth_corner: FacePoint,
    pub left_mouth_corner: FacePoint,
}

impl FaceLandmarks {
    pub const fn points(self) -> [FacePoint; 5] {
        [
            self.right_eye,
            self.left_eye,
            self.nose_tip,
            self.right_mouth_corner,
            self.left_mouth_corner,
        ]
    }
}

#[derive(Clone, PartialEq)]
pub struct FaceEmbedding {
    values: Box<[f32; SFACE_EMBEDDING_DIMENSIONS]>,
    space: String,
}

impl fmt::Debug for FaceEmbedding {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("FaceEmbedding")
            .field("values", &"<sensitive-biometric-redacted>")
            .field("dimensions", &SFACE_EMBEDDING_DIMENSIONS)
            .field("space", &self.space)
            .finish()
    }
}

impl FaceEmbedding {
    /// Admits one exact, L2-normalized `SFace` vector.
    ///
    /// # Errors
    ///
    /// Rejects the wrong dimension, non-finite values, an unversioned space,
    /// or a vector whose norm is not within the response tolerance.
    pub fn new(values: Vec<f32>, space: String) -> Result<Self, AnonymousPeopleGroupingError> {
        let values: Box<[f32; SFACE_EMBEDDING_DIMENSIONS]> = values
            .into_boxed_slice()
            .try_into()
            .map_err(|values: Box<[f32]>| {
            AnonymousPeopleGroupingError::InvalidEmbeddingDimensions(values.len())
        })?;
        if values.iter().any(|value| !value.is_finite()) {
            return Err(AnonymousPeopleGroupingError::InvalidEmbeddingValue);
        }
        if space.trim().is_empty() || space.len() > 512 {
            return Err(AnonymousPeopleGroupingError::InvalidEmbeddingSpace);
        }
        let squared_norm = values
            .iter()
            .map(|value| f64::from(*value) * f64::from(*value))
            .sum::<f64>();
        if (squared_norm.sqrt() - 1.0).abs() > 1.0e-3 {
            return Err(AnonymousPeopleGroupingError::EmbeddingNotNormalized);
        }
        Ok(Self { values, space })
    }

    pub fn space(&self) -> &str {
        &self.space
    }

    fn cosine_distance(&self, other: &Self) -> Option<f32> {
        if self.space != other.space {
            return None;
        }
        let similarity = self
            .values
            .iter()
            .zip(other.values.iter())
            .map(|(left, right)| left * right)
            .sum::<f32>()
            .clamp(-1.0, 1.0);
        Some(1.0 - similarity)
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct FaceOccurrenceId(String);

impl FaceOccurrenceId {
    /// # Errors
    ///
    /// Rejects empty, oversized, or non-portable occurrence identities.
    pub fn parse(value: impl Into<String>) -> Result<Self, AnonymousPeopleGroupingError> {
        let value = value.into();
        if value.is_empty()
            || value.len() > 256
            || !value.bytes().all(|byte| {
                byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_' | b':')
            })
        {
            return Err(AnonymousPeopleGroupingError::InvalidOccurrenceId);
        }
        Ok(Self(value))
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for FaceOccurrenceId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct FaceOccurrenceEvidence {
    pub occurrence_id: FaceOccurrenceId,
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub bounding_box: FaceBoundingBox,
    pub detection_confidence: f32,
    pub embedding: FaceEmbedding,
}

impl FaceOccurrenceEvidence {
    /// # Errors
    ///
    /// Rejects malformed geometry or confidence before it can affect grouping.
    pub fn validate(&self) -> Result<(), AnonymousPeopleGroupingError> {
        let box_values = [
            self.bounding_box.x,
            self.bounding_box.y,
            self.bounding_box.width,
            self.bounding_box.height,
        ];
        if box_values.iter().any(|value| !value.is_finite())
            || self.bounding_box.x < 0.0
            || self.bounding_box.y < 0.0
            || self.bounding_box.width <= 0.0
            || self.bounding_box.height <= 0.0
        {
            return Err(AnonymousPeopleGroupingError::InvalidFaceGeometry);
        }
        if !self.detection_confidence.is_finite()
            || !(0.0..=1.0).contains(&self.detection_confidence)
        {
            return Err(AnonymousPeopleGroupingError::InvalidDetectionConfidence);
        }
        Ok(())
    }

    fn reference(&self) -> FaceOccurrenceReference {
        FaceOccurrenceReference {
            occurrence_id: self.occurrence_id.clone(),
            photo_id: self.photo_id,
            representation_id: self.representation_id,
            bounding_box: self.bounding_box,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FaceOccurrenceReference {
    pub occurrence_id: FaceOccurrenceId,
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub bounding_box: FaceBoundingBox,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct AnonymousPersonGroupingPolicy {
    pub maximum_cosine_distance: f32,
    pub minimum_group_size: u16,
}

impl AnonymousPersonGroupingPolicy {
    /// # Errors
    ///
    /// Rejects a distance outside the normalized cosine range or a group size
    /// that could label a singleton as a matched person.
    pub fn new(
        maximum_cosine_distance: f32,
        minimum_group_size: u16,
    ) -> Result<Self, AnonymousPeopleGroupingError> {
        if !maximum_cosine_distance.is_finite() || !(0.0..=2.0).contains(&maximum_cosine_distance) {
            return Err(AnonymousPeopleGroupingError::InvalidMaximumDistance);
        }
        if minimum_group_size < 2 {
            return Err(AnonymousPeopleGroupingError::InvalidMinimumGroupSize(
                minimum_group_size,
            ));
        }
        Ok(Self {
            maximum_cosine_distance,
            minimum_group_size,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AnonymousPersonGroup {
    pub group_id: String,
    pub embedding_space: String,
    pub members: Vec<FaceOccurrenceReference>,
    /// Similarity medoid only; it is not a quality, identity, or naming claim.
    pub review_start: FaceOccurrenceId,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AnonymousPeopleGroupingPlan {
    pub grouping_revision: String,
    pub policy: AnonymousPersonGroupingPolicy,
    pub groups: Vec<AnonymousPersonGroup>,
    pub ungrouped: Vec<FaceOccurrenceReference>,
}

/// Builds deterministic complete-link candidate groups within one exact `SFace`
/// embedding space. Two faces from the same photo are never assigned to the
/// same anonymous person, preventing a strong co-occurrence contradiction.
///
/// This is a rebuildable review proposal, not recognition of a named person.
///
/// # Errors
///
/// Rejects malformed or duplicate evidence and oversized candidate sets.
pub fn propose_anonymous_people(
    occurrences: &[FaceOccurrenceEvidence],
    policy: AnonymousPersonGroupingPolicy,
) -> Result<AnonymousPeopleGroupingPlan, AnonymousPeopleGroupingError> {
    AnonymousPersonGroupingPolicy::new(policy.maximum_cosine_distance, policy.minimum_group_size)?;
    if occurrences.len() > MAX_FACE_OCCURRENCES {
        return Err(AnonymousPeopleGroupingError::TooManyOccurrences(
            occurrences.len(),
        ));
    }

    let mut by_id = BTreeMap::new();
    let mut by_space: BTreeMap<&str, Vec<&FaceOccurrenceEvidence>> = BTreeMap::new();
    for occurrence in occurrences {
        occurrence.validate()?;
        if by_id
            .insert(occurrence.occurrence_id.clone(), occurrence)
            .is_some()
        {
            return Err(AnonymousPeopleGroupingError::DuplicateOccurrence(
                occurrence.occurrence_id.clone(),
            ));
        }
        by_space
            .entry(occurrence.embedding.space())
            .or_default()
            .push(occurrence);
    }

    let mut groups = Vec::new();
    let mut ungrouped = Vec::new();
    for (space, mut candidates) in by_space {
        candidates.sort_by(|left, right| left.occurrence_id.cmp(&right.occurrence_id));
        let mut clusters: Vec<Vec<&FaceOccurrenceEvidence>> = Vec::new();
        for candidate in candidates {
            let compatible = clusters.iter().position(|cluster| {
                cluster.iter().all(|member| {
                    member.photo_id != candidate.photo_id
                        && member
                            .embedding
                            .cosine_distance(&candidate.embedding)
                            .is_some_and(|distance| distance <= policy.maximum_cosine_distance)
                })
            });
            if let Some(index) = compatible {
                clusters[index].push(candidate);
            } else {
                clusters.push(vec![candidate]);
            }
        }

        for mut cluster in clusters {
            cluster.sort_by(|left, right| left.occurrence_id.cmp(&right.occurrence_id));
            if cluster.len() < usize::from(policy.minimum_group_size) {
                ungrouped.extend(cluster.into_iter().map(FaceOccurrenceEvidence::reference));
                continue;
            }
            let review_start = similarity_medoid(&cluster);
            groups.push(AnonymousPersonGroup {
                group_id: group_id(space, &cluster),
                embedding_space: space.to_owned(),
                members: cluster
                    .into_iter()
                    .map(FaceOccurrenceEvidence::reference)
                    .collect(),
                review_start,
            });
        }
    }

    groups.sort_by(|left, right| left.group_id.cmp(&right.group_id));
    ungrouped.sort_by(|left, right| left.occurrence_id.cmp(&right.occurrence_id));
    Ok(AnonymousPeopleGroupingPlan {
        grouping_revision: ANONYMOUS_PEOPLE_GROUPING_REVISION.into(),
        policy,
        groups,
        ungrouped,
    })
}

fn similarity_medoid(cluster: &[&FaceOccurrenceEvidence]) -> FaceOccurrenceId {
    cluster
        .iter()
        .min_by(|left, right| {
            total_distance(left, cluster)
                .total_cmp(&total_distance(right, cluster))
                .then_with(|| left.occurrence_id.cmp(&right.occurrence_id))
        })
        .expect("validated cluster is non-empty")
        .occurrence_id
        .clone()
}

fn total_distance(candidate: &FaceOccurrenceEvidence, cluster: &[&FaceOccurrenceEvidence]) -> f64 {
    cluster
        .iter()
        .filter(|other| other.occurrence_id != candidate.occurrence_id)
        .map(|other| {
            f64::from(
                candidate
                    .embedding
                    .cosine_distance(&other.embedding)
                    .expect("one cluster contains one embedding space"),
            )
        })
        .sum()
}

fn group_id(space: &str, cluster: &[&FaceOccurrenceEvidence]) -> String {
    let mut hasher = blake3::Hasher::new();
    hash_field(&mut hasher, ANONYMOUS_PEOPLE_GROUPING_REVISION.as_bytes());
    hash_field(&mut hasher, space.as_bytes());
    for occurrence in cluster {
        hash_field(&mut hasher, occurrence.occurrence_id.as_str().as_bytes());
    }
    format!("anonymous-person-{}", hasher.finalize().to_hex())
}

fn hash_field(hasher: &mut blake3::Hasher, value: &[u8]) {
    hasher.update(&(value.len() as u64).to_le_bytes());
    hasher.update(value);
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum AnonymousPeopleGroupingError {
    #[error("SFace embedding must have {SFACE_EMBEDDING_DIMENSIONS} dimensions, got {0}")]
    InvalidEmbeddingDimensions(usize),
    #[error("SFace embedding contains a non-finite value")]
    InvalidEmbeddingValue,
    #[error("SFace embedding is not L2-normalized")]
    EmbeddingNotNormalized,
    #[error("SFace embedding space must be non-empty and at most 512 bytes")]
    InvalidEmbeddingSpace,
    #[error("face occurrence identity is not portable bounded text")]
    InvalidOccurrenceId,
    #[error("face occurrence geometry is invalid")]
    InvalidFaceGeometry,
    #[error("face detection confidence must be finite and in 0..=1")]
    InvalidDetectionConfidence,
    #[error("maximum cosine distance must be finite and in 0..=2")]
    InvalidMaximumDistance,
    #[error("minimum anonymous group size must be at least 2, got {0}")]
    InvalidMinimumGroupSize(u16),
    #[error("face occurrence set exceeds {MAX_FACE_OCCURRENCES}, got {0}")]
    TooManyOccurrences(usize),
    #[error("face occurrence {0} appears more than once")]
    DuplicateOccurrence(FaceOccurrenceId),
}

#[cfg(test)]
mod tests;
