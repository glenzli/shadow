//! Application-owned orchestration for transient anonymous-person analysis.
//!
//! The Catalog chooses exact current visuals, the cache verifies their bytes,
//! infer-runtime supplies typed YuNet/SFace evidence, and this owner rejects
//! stale results before producing an in-memory grouping proposal.

mod thumbnail;

use std::{collections::BTreeMap, fmt::Write as _};

use serde::Serialize;
use shadow_ai::{
    AnonymousPeopleGroupingError, AnonymousPeopleGroupingPlan, AnonymousPersonGroupingPolicy,
    FaceAnalysisProvider, FaceOccurrenceEvidence, FaceOccurrenceId, InferRuntimeClientError,
    MAX_FACE_OCCURRENCES, propose_anonymous_people,
};
use shadow_cache::{BlobDigest, ContentAddressedStore};
use shadow_catalog::{CachedArtifactRecord, CatalogError, CatalogHandle, ReviewCursor};
use shadow_domain::PreviewCodec;
use thiserror::Error;

pub const DEFAULT_PEOPLE_MAXIMUM_COSINE_DISTANCE: f32 = 0.35;
const DEFAULT_MAXIMUM_PHOTOS: usize = 512;
const DEFAULT_MAXIMUM_FACES: usize = 2_048;
const REVIEW_PAGE_SIZE: usize = 128;

#[derive(Debug, Copy, Clone, PartialEq)]
pub struct PeopleAnalysisPolicy {
    pub grouping: AnonymousPersonGroupingPolicy,
    pub minimum_detection_confidence: f32,
    pub maximum_photos: usize,
    pub maximum_faces: usize,
}

impl Default for PeopleAnalysisPolicy {
    fn default() -> Self {
        Self {
            grouping: AnonymousPersonGroupingPolicy {
                maximum_cosine_distance: DEFAULT_PEOPLE_MAXIMUM_COSINE_DISTANCE,
                minimum_group_size: 2,
            },
            minimum_detection_confidence: 0.75,
            maximum_photos: DEFAULT_MAXIMUM_PHOTOS,
            maximum_faces: DEFAULT_MAXIMUM_FACES,
        }
    }
}

impl PeopleAnalysisPolicy {
    fn validate(self) -> Result<Self, PeopleAnalysisError> {
        AnonymousPersonGroupingPolicy::new(
            self.grouping.maximum_cosine_distance,
            self.grouping.minimum_group_size,
        )?;
        if !self.minimum_detection_confidence.is_finite()
            || !(0.0..=1.0).contains(&self.minimum_detection_confidence)
            || self.maximum_photos == 0
            || self.maximum_faces == 0
            || self.maximum_faces > MAX_FACE_OCCURRENCES
        {
            return Err(PeopleAnalysisError::InvalidPolicy);
        }
        Ok(self)
    }
}

#[derive(Debug, Default, Copy, Clone, Eq, PartialEq, Serialize)]
pub struct PeopleAnalysisSkipped {
    pub no_current_visual: usize,
    pub unsupported_visual: usize,
    pub stale_input: usize,
    pub low_detection_confidence: usize,
    pub ineligible_embedding: usize,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct PeopleAnalysisReport {
    pub analyzed_photos: usize,
    pub detected_faces: usize,
    pub embedded_faces: usize,
    pub truncated: bool,
    pub skipped: PeopleAnalysisSkipped,
    pub grouping: AnonymousPeopleGroupingPlan,
    /// Bounded, request-local face crops for the representative member of
    /// each visible group. They are presentation payloads and never serialize
    /// into CLI output, Catalog state, or the rebuildable grouping plan.
    #[serde(skip_serializing)]
    pub group_previews: Vec<PeopleGroupPreview>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct PeopleGroupPreview {
    pub group_id: String,
    pub thumbnail_jpeg: Vec<u8>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum PeopleAnalysisPhase {
    Reviewing,
    Grouping,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct PeopleAnalysisProgress {
    pub phase: PeopleAnalysisPhase,
    pub analyzed_photos: usize,
    pub maximum_photos: usize,
    pub detected_faces: usize,
    pub compared_faces: usize,
}

/// Cooperative lifecycle boundary for one bounded people-analysis job.
///
/// Provider calls are synchronous. Cancellation is therefore observed before
/// and after each call, never represented as interrupting an in-flight model
/// request.
pub trait PeopleAnalysisControl: Send + Sync {
    fn cancellation_requested(&self) -> bool;
    fn publish(&self, progress: PeopleAnalysisProgress);
}

#[derive(Debug)]
struct UnobservedPeopleAnalysis;

impl PeopleAnalysisControl for UnobservedPeopleAnalysis {
    fn cancellation_requested(&self) -> bool {
        false
    }

    fn publish(&self, _progress: PeopleAnalysisProgress) {}
}

/// Analyzes a bounded, stable Review ordering without persisting embeddings.
///
/// The function rechecks each selected artifact after all model calls for that
/// photo. A late result for replaced source bytes or a new working Recipe is
/// discarded as a unit.
///
/// # Errors
///
/// Returns policy, Catalog/cache integrity, provider, or grouping failures.
pub fn analyze_review_people(
    catalog: &CatalogHandle,
    cache_root: impl Into<std::path::PathBuf>,
    provider: &impl FaceAnalysisProvider,
    policy: PeopleAnalysisPolicy,
) -> Result<PeopleAnalysisReport, PeopleAnalysisError> {
    analyze_review_people_with_control(
        catalog,
        cache_root,
        provider,
        policy,
        &UnobservedPeopleAnalysis,
    )
}

/// Runs the same transient analysis with observable progress and cooperative
/// cancellation owned by the caller's session-local job lifecycle.
pub fn analyze_review_people_with_control(
    catalog: &CatalogHandle,
    cache_root: impl Into<std::path::PathBuf>,
    provider: &impl FaceAnalysisProvider,
    policy: PeopleAnalysisPolicy,
    control: &(impl PeopleAnalysisControl + ?Sized),
) -> Result<PeopleAnalysisReport, PeopleAnalysisError> {
    let policy = policy.validate()?;
    let cache = ContentAddressedStore::open(cache_root.into())?;
    let mut occurrences = Vec::new();
    let mut cursor: Option<ReviewCursor> = None;
    let mut analyzed_photos = 0;
    let mut detected_faces = 0;
    let mut compared_faces = 0;
    let mut skipped = PeopleAnalysisSkipped::default();
    let mut truncated = false;
    let mut occurrence_thumbnails = BTreeMap::new();
    let mut resident_thumbnail_bytes = 0_usize;

    publish_progress(
        control,
        PeopleAnalysisPhase::Reviewing,
        analyzed_photos,
        policy.maximum_photos,
        detected_faces,
        compared_faces,
    )?;

    'pages: loop {
        ensure_active(control)?;
        let page = catalog.review_page(cursor.as_ref(), REVIEW_PAGE_SIZE)?;
        if page.items.is_empty() {
            break;
        }
        for item in page.items {
            ensure_active(control)?;
            if analyzed_photos == policy.maximum_photos {
                truncated = true;
                break 'pages;
            }
            analyzed_photos += 1;
            publish_progress(
                control,
                PeopleAnalysisPhase::Reviewing,
                analyzed_photos,
                policy.maximum_photos,
                detected_faces,
                compared_faces,
            )?;
            let Some(record) = item.visual else {
                skipped.no_current_visual += 1;
                continue;
            };
            if record.artifact.codec != PreviewCodec::Jpeg {
                skipped.unsupported_visual += 1;
                continue;
            }
            let image = read_verified_visual(&cache, &record)?;
            let source_revision = source_revision(item.photo_id, &record);
            ensure_active(control)?;
            let detection = provider.detect_faces(&image, "image/jpeg", &source_revision)?;
            ensure_active(control)?;
            if detection.width != record.artifact.dimensions.width
                || detection.height != record.artifact.dimensions.height
            {
                return Err(PeopleAnalysisError::ProviderGeometryMismatch);
            }
            detected_faces += detection.detections.len();
            publish_progress(
                control,
                PeopleAnalysisPhase::Reviewing,
                analyzed_photos,
                policy.maximum_photos,
                detected_faces,
                compared_faces,
            )?;
            let mut current_occurrences = Vec::new();
            let mut thumbnail_source = None;
            let mut thumbnail_decode_attempted = false;
            for (ordinal, face) in detection.detections.into_iter().enumerate() {
                if face.confidence < policy.minimum_detection_confidence {
                    skipped.low_detection_confidence += 1;
                    continue;
                }
                if occurrences.len() + current_occurrences.len() == policy.maximum_faces {
                    truncated = true;
                    break 'pages;
                }
                ensure_active(control)?;
                let embedded = match provider.embed_face(
                    &image,
                    "image/jpeg",
                    &source_revision,
                    face.landmarks,
                ) {
                    Ok(embedded) => embedded,
                    Err(InferRuntimeClientError::Api { status, .. }) if status.as_u16() == 400 => {
                        skipped.ineligible_embedding += 1;
                        continue;
                    }
                    Err(error) => return Err(error.into()),
                };
                ensure_active(control)?;
                compared_faces += 1;
                publish_progress(
                    control,
                    PeopleAnalysisPhase::Reviewing,
                    analyzed_photos,
                    policy.maximum_photos,
                    detected_faces,
                    compared_faces,
                )?;
                let occurrence_id = occurrence_id(
                    &source_revision,
                    ordinal,
                    &face,
                    &detection.provenance,
                    &embedded.provenance,
                )?;
                if !thumbnail_decode_attempted {
                    thumbnail_decode_attempted = true;
                    if thumbnail::source_dimensions_admitted(
                        record.artifact.dimensions.width,
                        record.artifact.dimensions.height,
                    ) {
                        thumbnail_source = image::load_from_memory(&image).ok();
                    }
                }
                if let Some(source) = thumbnail_source.as_ref()
                    && let Some(thumbnail) =
                        thumbnail::face_thumbnail_jpeg(source, face.bounding_box)
                    && resident_thumbnail_bytes.saturating_add(thumbnail.len())
                        <= thumbnail::MAX_RESIDENT_PEOPLE_THUMBNAIL_BYTES
                {
                    resident_thumbnail_bytes += thumbnail.len();
                    occurrence_thumbnails.insert(occurrence_id.clone(), thumbnail);
                }
                current_occurrences.push(FaceOccurrenceEvidence {
                    occurrence_id,
                    photo_id: item.photo_id,
                    representation_id: item.representation_id,
                    bounding_box: face.bounding_box,
                    detection_confidence: face.confidence,
                    embedding: embedded.embedding,
                });
            }
            if catalog.preferred_cached_artifact(item.representation_id)? != Some(record) {
                skipped.stale_input += 1;
                continue;
            }
            occurrences.extend(current_occurrences);
        }
        cursor = page.next_cursor;
        if cursor.is_none() {
            break;
        }
    }

    publish_progress(
        control,
        PeopleAnalysisPhase::Grouping,
        analyzed_photos,
        policy.maximum_photos,
        detected_faces,
        compared_faces,
    )?;
    let grouping = propose_anonymous_people(&occurrences, policy.grouping)?;
    ensure_active(control)?;
    let group_previews = grouping
        .groups
        .iter()
        .filter_map(|group| {
            occurrence_thumbnails
                .remove(&group.review_start)
                .map(|thumbnail_jpeg| PeopleGroupPreview {
                    group_id: group.group_id.clone(),
                    thumbnail_jpeg,
                })
        })
        .collect();
    Ok(PeopleAnalysisReport {
        analyzed_photos,
        detected_faces,
        embedded_faces: occurrences.len(),
        truncated,
        skipped,
        grouping,
        group_previews,
    })
}

fn ensure_active(
    control: &(impl PeopleAnalysisControl + ?Sized),
) -> Result<(), PeopleAnalysisError> {
    if control.cancellation_requested() {
        return Err(PeopleAnalysisError::Cancelled);
    }
    Ok(())
}

fn publish_progress(
    control: &(impl PeopleAnalysisControl + ?Sized),
    phase: PeopleAnalysisPhase,
    analyzed_photos: usize,
    maximum_photos: usize,
    detected_faces: usize,
    compared_faces: usize,
) -> Result<(), PeopleAnalysisError> {
    ensure_active(control)?;
    control.publish(PeopleAnalysisProgress {
        phase,
        analyzed_photos,
        maximum_photos,
        detected_faces,
        compared_faces,
    });
    ensure_active(control)
}

fn read_verified_visual(
    cache: &ContentAddressedStore,
    record: &CachedArtifactRecord,
) -> Result<Vec<u8>, PeopleAnalysisError> {
    if record.artifact.blob_algorithm != "blake3-256" {
        return Err(PeopleAnalysisError::UnsupportedBlobAlgorithm(
            record.artifact.blob_algorithm.clone(),
        ));
    }
    let bytes = cache.read_verified(BlobDigest::from_bytes(record.artifact.blob_digest))?;
    if u64::try_from(bytes.len()).unwrap_or(u64::MAX) != record.artifact.blob_byte_len {
        return Err(PeopleAnalysisError::BlobLengthMismatch);
    }
    Ok(bytes)
}

fn source_revision(photo_id: shadow_domain::PhotoId, record: &CachedArtifactRecord) -> String {
    let mut digest = String::with_capacity(64);
    for byte in record.artifact.blob_digest {
        write!(&mut digest, "{byte:02x}").expect("writing into String cannot fail");
    }
    format!(
        "shadow:{}/representation:{}/source:{}-{}/artifact:{}",
        photo_id,
        record.representation_id,
        record.source.byte_len,
        record
            .source
            .modified_at_ms
            .map_or_else(|| "none".into(), |value| value.to_string()),
        digest
    )
}

fn occurrence_id(
    source_revision: &str,
    ordinal: usize,
    face: &shadow_ai::DetectedFace,
    detector: &shadow_ai::VisionProvenance,
    embedder: &shadow_ai::VisionProvenance,
) -> Result<FaceOccurrenceId, PeopleAnalysisError> {
    let mut hasher = blake3::Hasher::new();
    hash_field(&mut hasher, b"shadow.face-occurrence.20260810.1");
    hash_field(&mut hasher, source_revision.as_bytes());
    hash_field(&mut hasher, &ordinal.to_le_bytes());
    for value in [
        detector.model_build.as_str(),
        detector.artifact_sha256.as_str(),
        detector.preprocessing_identity.as_str(),
        detector.postprocessing_identity.as_str(),
        embedder.model_build.as_str(),
        embedder.artifact_sha256.as_str(),
        embedder.preprocessing_identity.as_str(),
        embedder.postprocessing_identity.as_str(),
    ] {
        hash_field(&mut hasher, value.as_bytes());
    }
    for value in [
        face.bounding_box.x,
        face.bounding_box.y,
        face.bounding_box.width,
        face.bounding_box.height,
        face.confidence,
    ] {
        hash_field(&mut hasher, &value.to_bits().to_le_bytes());
    }
    for point in face.landmarks.points() {
        hash_field(&mut hasher, &point.x.to_bits().to_le_bytes());
        hash_field(&mut hasher, &point.y.to_bits().to_le_bytes());
    }
    Ok(FaceOccurrenceId::parse(format!(
        "face-occurrence-{}",
        hasher.finalize().to_hex()
    ))?)
}

fn hash_field(hasher: &mut blake3::Hasher, value: &[u8]) {
    hasher.update(&(value.len() as u64).to_le_bytes());
    hasher.update(value);
}

#[derive(Debug, Error)]
pub enum PeopleAnalysisError {
    #[error("anonymous-person analysis was cancelled")]
    Cancelled,
    #[error("anonymous-person analysis policy is invalid")]
    InvalidPolicy,
    #[error("infer-runtime image geometry disagrees with the selected Catalog artifact")]
    ProviderGeometryMismatch,
    #[error("unsupported cached blob algorithm: {0}")]
    UnsupportedBlobAlgorithm(String),
    #[error("cached visual byte length disagrees with Catalog metadata")]
    BlobLengthMismatch,
    #[error(transparent)]
    Catalog(#[from] CatalogError),
    #[error(transparent)]
    Cache(#[from] shadow_cache::CacheError),
    #[error(transparent)]
    Provider(#[from] InferRuntimeClientError),
    #[error(transparent)]
    Grouping(#[from] AnonymousPeopleGroupingError),
}

#[cfg(test)]
mod tests;
