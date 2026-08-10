//! Rebuildable, multi-label semantic classification over Review visuals.
//!
//! Category definitions remain user-owned configuration. This owner embeds
//! those descriptions and current display artifacts into one exact semantic
//! space, persists only a rebuildable sidecar index, and returns anonymous
//! membership projections without promoting them to Library keywords.

mod adaptation;
mod index;

use std::{collections::HashMap, fmt::Write as _};

use blake3::Hasher;
use serde::Serialize;
use shadow_ai::{
    InferRuntimeClientError, SemanticEmbedding, SemanticEmbeddingProvider, SemanticRequestPriority,
};
use shadow_catalog::{CachedArtifactRecord, CatalogError, CatalogHandle};
use shadow_domain::{PhotoId, PreviewCodec, RepresentationId};
use thiserror::Error;

use crate::{CachedArtifactLoadError, CachedArtifactLoader};
use adaptation::{AdaptiveCategoryModel, FeedbackExample, FeedbackLabel};
use index::{CachedEmbedding, SemanticIndex, SemanticIndexError};

pub const DEFAULT_SMART_CLASSIFICATION_BATCH_SIZE: usize = 24;
pub const MAX_SMART_CLASSIFICATION_BATCH_SIZE: usize = 64;
pub const MAX_SMART_CATEGORY_DEFINITIONS: usize = 32;
const MAXIMUM_COMPETITIVE_SCORE_GAP: f32 = 0.0125;
const HIGH_VALUE_UNCERTAINTY_BAND: f32 = 0.006;
const MAXIMUM_REVIEW_QUEUE_ITEMS: usize = 512;

#[derive(Debug, Clone, PartialEq)]
pub struct SmartCategoryDefinition {
    pub id: String,
    pub description: String,
    pub minimum_similarity: f32,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct SmartClassificationPolicy {
    pub batch_size: usize,
}

impl Default for SmartClassificationPolicy {
    fn default() -> Self {
        Self {
            batch_size: DEFAULT_SMART_CLASSIFICATION_BATCH_SIZE,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SmartCategoryMatch {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub source_revision: String,
    pub category_id: String,
    pub cosine_similarity: f32,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize)]
pub enum SmartCategoryFeedbackDecision {
    Clear,
    DoesNotBelong,
    Belongs,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SmartCategoryReviewItem {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub category_id: String,
    pub adapted_similarity: f32,
    pub decision_margin: f32,
}

#[derive(Debug, Clone, PartialEq)]
struct SmartClassifiedPhoto {
    photo_id: PhotoId,
    representation_id: RepresentationId,
    source_revision: String,
}

#[derive(Debug, Clone, PartialEq)]
struct SmartCategoryUncertainty {
    photo_id: PhotoId,
    representation_id: RepresentationId,
    source_revision: String,
    category_id: String,
    adapted_similarity: f32,
    decision_margin: f32,
}

#[derive(Debug, Clone, PartialEq)]
pub struct SmartClassificationRequest {
    pub config_revision: String,
    pub generation: Option<String>,
    pub start_new: bool,
    pub clear_embeddings: bool,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize)]
pub enum SmartClassificationStatus {
    Empty,
    Running,
    Paused,
    Complete,
    Failed,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SmartCategoryCount {
    pub category_id: String,
    pub count: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SmartClassificationSnapshot {
    pub config_revision: String,
    pub generation: String,
    pub status: SmartClassificationStatus,
    pub embedding_space: String,
    pub model_build: String,
    pub processed_photos: u64,
    pub total_photos: u64,
    pub updated_at_ms: i64,
    pub has_published_results: bool,
    pub published_config_revision: String,
    pub published_at_ms: i64,
    pub category_counts: Vec<SmartCategoryCount>,
    pub uncertain_photos: u64,
    pub adaptation_pending: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SmartClassificationBatch {
    pub config_revision: String,
    pub generation: String,
    pub status: SmartClassificationStatus,
    pub embedding_space: String,
    pub model_build: String,
    pub processed_photos: u64,
    pub embedded_photos: usize,
    pub reused_photos: usize,
    pub skipped_photos: usize,
    pub total_photos: u64,
}

/// Classifies one bounded Review page and checkpoints its image vectors.
///
/// A new request creates a durable generation. Every continuation names that
/// exact generation, so a stale caller cannot silently start or append to a
/// different scan. The first resolved embedding space is checkpointed and all
/// later batches must remain in that exact space.
///
/// # Errors
///
/// Returns validation, Catalog/cache-integrity, derived-index, provider, or
/// exact-space errors. Callers must discard an incomplete scan on any error.
pub fn classify_review_smart_categories(
    catalog: &CatalogHandle,
    cache_root: impl Into<std::path::PathBuf>,
    provider: &impl SemanticEmbeddingProvider,
    definitions: &[SmartCategoryDefinition],
    request: &SmartClassificationRequest,
    policy: SmartClassificationPolicy,
) -> Result<SmartClassificationBatch, SmartClassificationError> {
    validate_request(definitions, policy)?;
    validate_config_revision(&request.config_revision)?;
    if request.clear_embeddings && !request.start_new {
        return Err(SmartClassificationError::InvalidRequest);
    }
    let cache_root = cache_root.into();
    let mut index = SemanticIndex::open(&cache_root)?;
    let checkpoint = index.prepare_run(
        &request.config_revision,
        request.generation.as_deref(),
        request.start_new,
        request.clear_embeddings,
    )?;
    let result = classify_prepared_batch(
        catalog,
        &cache_root,
        provider,
        definitions,
        policy,
        &mut index,
        &checkpoint,
    );
    if result.is_err() {
        let _ = index.mark_failed(&checkpoint.generation);
    }
    result
}

fn classify_prepared_batch(
    catalog: &CatalogHandle,
    cache_root: &std::path::Path,
    provider: &impl SemanticEmbeddingProvider,
    definitions: &[SmartCategoryDefinition],
    policy: SmartClassificationPolicy,
    index: &mut SemanticIndex,
    checkpoint: &index::RunCheckpoint,
) -> Result<SmartClassificationBatch, SmartClassificationError> {
    let expected_space =
        (!checkpoint.embedding_space.is_empty()).then_some(checkpoint.embedding_space.as_str());
    let (category_embeddings, embedding_space, model_build) =
        category_embeddings(index, provider, definitions, expected_space)?;
    let (adaptive_models, explicit_feedback) = adaptive_models(
        definitions,
        &category_embeddings,
        index.stored_feedback(&embedding_space)?,
    );
    let loader = CachedArtifactLoader::open(catalog.clone(), &cache_root)?;
    let page = catalog.review_page(checkpoint.cursor.as_ref(), policy.batch_size)?;
    let processed_photos = page.items.len();
    let mut embedded_photos = 0;
    let mut reused_photos = 0;
    let mut skipped_photos = 0;
    let mut matches = Vec::new();
    let mut classified_photos = Vec::new();
    let mut uncertainties = Vec::new();

    for item in page.items {
        let Some(record) = item.visual else {
            skipped_photos += 1;
            continue;
        };
        if record.artifact.codec != PreviewCodec::Jpeg {
            skipped_photos += 1;
            continue;
        }
        let source_revision = semantic_source_revision(item.photo_id, &record);
        let image_embedding =
            if let Some(cached) = index.image(&source_revision, &embedding_space)? {
                reused_photos += 1;
                cached.embedding
            } else {
                let image = loader.load_bytes(&record)?;
                let evidence = provider.embed_image_semantics(
                    &image,
                    "image/jpeg",
                    &source_revision,
                    SemanticRequestPriority::Background,
                )?;
                if evidence.width != record.artifact.dimensions.width
                    || evidence.height != record.artifact.dimensions.height
                {
                    return Err(SmartClassificationError::ProviderGeometryMismatch);
                }
                if evidence.embedding.space().space_id() != embedding_space {
                    return Err(SmartClassificationError::EmbeddingSpaceChanged);
                }
                index.store_image(
                    &source_revision,
                    &evidence.embedding,
                    &evidence.provenance.model_build,
                )?;
                embedded_photos += 1;
                evidence.embedding
            };
        if catalog.preferred_cached_artifact(item.representation_id)? != Some(record) {
            skipped_photos += 1;
            continue;
        }
        classified_photos.push(SmartClassifiedPhoto {
            photo_id: item.photo_id,
            representation_id: item.representation_id,
            source_revision: source_revision.clone(),
        });
        let scores = adaptive_models
            .iter()
            .map(|model| model.score(image_embedding.values()))
            .collect::<Vec<_>>();
        let adapted_similarities = scores
            .iter()
            .map(|score| score.adapted_similarity)
            .collect::<Vec<_>>();
        let best_similarity = adapted_similarities
            .iter()
            .copied()
            .max_by(f32::total_cmp)
            .expect("validated non-empty definitions");
        let competitive = competitive_category_indices(definitions, &adapted_similarities);
        for (category_index, definition) in definitions.iter().enumerate() {
            let feedback_key = (
                definition.id.clone(),
                item.photo_id.to_string(),
                item.representation_id.to_string(),
            );
            let explicit = explicit_feedback.get(&feedback_key).copied();
            let inferred_member = competitive.contains(&category_index);
            if explicit == Some(FeedbackLabel::Belongs) || (explicit.is_none() && inferred_member) {
                matches.push(SmartCategoryMatch {
                    photo_id: item.photo_id,
                    representation_id: item.representation_id,
                    source_revision: source_revision.clone(),
                    category_id: definition.id.clone(),
                    cosine_similarity: scores[category_index].adapted_similarity,
                });
            }
            if explicit.is_some() {
                continue;
            }
            let absolute_margin =
                scores[category_index].adapted_similarity - definition.minimum_similarity;
            let competitive_margin = scores[category_index].adapted_similarity
                - (best_similarity - MAXIMUM_COMPETITIVE_SCORE_GAP);
            let decision_margin = absolute_margin.min(competitive_margin);
            if absolute_margin >= -HIGH_VALUE_UNCERTAINTY_BAND
                && competitive_margin >= -HIGH_VALUE_UNCERTAINTY_BAND
                && decision_margin <= HIGH_VALUE_UNCERTAINTY_BAND
            {
                uncertainties.push(SmartCategoryUncertainty {
                    photo_id: item.photo_id,
                    representation_id: item.representation_id,
                    source_revision: source_revision.clone(),
                    category_id: definition.id.clone(),
                    adapted_similarity: scores[category_index].adapted_similarity,
                    decision_margin,
                });
            }
        }
    }

    let updated = index.checkpoint_batch(
        checkpoint,
        &embedding_space,
        &model_build,
        processed_photos,
        page.total_items,
        page.next_cursor.as_ref(),
        &matches,
        &classified_photos,
        &uncertainties,
    )?;
    Ok(SmartClassificationBatch {
        config_revision: updated.config_revision,
        generation: updated.generation,
        status: parse_status(&updated.status)?,
        embedding_space,
        model_build,
        processed_photos: updated.processed_photos,
        embedded_photos,
        reused_photos,
        skipped_photos,
        total_photos: page.total_items,
    })
}

fn competitive_category_indices(
    definitions: &[SmartCategoryDefinition],
    similarities: &[f32],
) -> Vec<usize> {
    debug_assert_eq!(definitions.len(), similarities.len());
    let Some(best_similarity) = similarities.iter().copied().max_by(f32::total_cmp) else {
        return Vec::new();
    };
    definitions
        .iter()
        .zip(similarities)
        .enumerate()
        .filter_map(|(index, (definition, similarity))| {
            (*similarity >= definition.minimum_similarity
                && *similarity >= best_similarity - MAXIMUM_COMPETITIVE_SCORE_GAP)
                .then_some(index)
        })
        .collect()
}

/// Loads resumable task metadata and the last atomically published category counts.
///
/// # Errors
///
/// Returns an index error when the rebuildable sidecar cannot be opened or decoded.
pub fn smart_classification_snapshot(
    cache_root: impl AsRef<std::path::Path>,
) -> Result<SmartClassificationSnapshot, SmartClassificationError> {
    let index = SemanticIndex::open(cache_root.as_ref())?;
    let run = index.run_checkpoint()?;
    let published = index.published_snapshot()?;
    let category_counts = published
        .as_ref()
        .map(|snapshot| {
            snapshot
                .category_counts
                .iter()
                .map(|(category_id, count)| SmartCategoryCount {
                    category_id: category_id.clone(),
                    count: *count,
                })
                .collect()
        })
        .unwrap_or_default();
    Ok(SmartClassificationSnapshot {
        config_revision: run
            .as_ref()
            .map(|value| value.config_revision.clone())
            .unwrap_or_default(),
        generation: run
            .as_ref()
            .map(|value| value.generation.clone())
            .unwrap_or_default(),
        status: run
            .as_ref()
            .map(|value| parse_status(&value.status))
            .transpose()?
            .unwrap_or(SmartClassificationStatus::Empty),
        embedding_space: run
            .as_ref()
            .map(|value| value.embedding_space.clone())
            .unwrap_or_default(),
        model_build: run
            .as_ref()
            .map(|value| value.model_build.clone())
            .unwrap_or_default(),
        processed_photos: run.as_ref().map_or(0, |value| value.processed_photos),
        total_photos: run.as_ref().map_or(0, |value| value.total_photos),
        updated_at_ms: run.as_ref().map_or(0, |value| value.updated_at_ms),
        has_published_results: published.is_some(),
        published_config_revision: published
            .as_ref()
            .map(|value| value.config_revision.clone())
            .unwrap_or_default(),
        published_at_ms: published.as_ref().map_or(0, |value| value.published_at_ms),
        category_counts,
        uncertain_photos: index.published_uncertain_photo_count()?,
        adaptation_pending: index.adaptation_pending()?,
    })
}

/// Loads exact members of one category from the last complete published generation.
///
/// # Errors
///
/// Returns an index error when the rebuildable sidecar cannot be queried.
pub fn smart_category_members(
    cache_root: impl AsRef<std::path::Path>,
    category_id: &str,
) -> Result<Vec<String>, SmartClassificationError> {
    if category_id.is_empty() || category_id.len() > 80 {
        return Err(SmartClassificationError::InvalidRequest);
    }
    Ok(SemanticIndex::open(cache_root.as_ref())?.published_members(category_id)?)
}

/// Loads the bounded, highest-value uncertainty queue from the published generation.
///
/// # Errors
///
/// Returns an index error when the rebuildable sidecar cannot be queried.
pub fn smart_category_review_queue(
    cache_root: impl AsRef<std::path::Path>,
) -> Result<Vec<SmartCategoryReviewItem>, SmartClassificationError> {
    Ok(SemanticIndex::open(cache_root.as_ref())?.published_review_queue()?)
}

/// Records or clears an explicit user decision and applies it immediately to
/// the currently published projection. The next rebuild uses it as adaptive
/// evidence for nearby photos.
///
/// # Errors
///
/// Returns validation or index errors when the category is invalid or the
/// photo is not part of the published classification generation.
pub fn set_smart_category_feedback(
    cache_root: impl AsRef<std::path::Path>,
    photo_id: PhotoId,
    representation_id: RepresentationId,
    category_id: &str,
    decision: SmartCategoryFeedbackDecision,
) -> Result<(), SmartClassificationError> {
    validate_category_id(category_id)?;
    SemanticIndex::open(cache_root.as_ref())?.set_user_feedback(
        &photo_id.to_string(),
        &representation_id.to_string(),
        category_id,
        decision,
    )?;
    Ok(())
}

/// Marks the current generation paused after its in-flight bounded batch finishes.
///
/// # Errors
///
/// Returns an index error when the checkpoint cannot be updated.
pub fn pause_smart_classification(
    cache_root: impl AsRef<std::path::Path>,
    generation: &str,
) -> Result<(), SmartClassificationError> {
    if generation.is_empty() || generation.len() > 128 {
        return Err(SmartClassificationError::InvalidRequest);
    }
    Ok(SemanticIndex::open(cache_root.as_ref())?.mark_paused(generation)?)
}

fn category_embeddings(
    index: &SemanticIndex,
    provider: &impl SemanticEmbeddingProvider,
    definitions: &[SmartCategoryDefinition],
    expected_space: Option<&str>,
) -> Result<(Vec<SemanticEmbedding>, String, String), SmartClassificationError> {
    let mut selected_space = expected_space.map(str::to_owned);
    let mut selected_build = String::new();
    let mut embeddings = Vec::with_capacity(definitions.len());
    for definition in definitions {
        let revision = category_revision(definition);
        let cached = selected_space
            .as_deref()
            .and_then(|space| index.text(&revision, space).transpose())
            .transpose()?;
        let CachedEmbedding {
            embedding,
            model_build,
        } = if let Some(cached) = cached {
            cached
        } else {
            let evidence = provider.embed_text_semantics(
                &definition.description,
                &revision,
                Some("en"),
                SemanticRequestPriority::Background,
            )?;
            if let Some(space) = selected_space.as_deref()
                && evidence.embedding.space().space_id() != space
            {
                return Err(SmartClassificationError::EmbeddingSpaceChanged);
            }
            selected_space = Some(evidence.embedding.space().space_id().to_owned());
            index.store_text(
                &revision,
                &evidence.embedding,
                &evidence.provenance.model_build,
            )?;
            CachedEmbedding {
                embedding: evidence.embedding,
                model_build: evidence.provenance.model_build,
            }
        };
        if selected_build.is_empty() {
            selected_build = model_build;
        }
        embeddings.push(embedding);
    }
    Ok((
        embeddings,
        selected_space.expect("validated non-empty definitions"),
        selected_build,
    ))
}

type ExplicitFeedback = HashMap<(String, String, String), FeedbackLabel>;

fn adaptive_models(
    definitions: &[SmartCategoryDefinition],
    category_embeddings: &[SemanticEmbedding],
    stored: Vec<index::StoredFeedback>,
) -> (Vec<AdaptiveCategoryModel>, ExplicitFeedback) {
    let mut examples_by_category: HashMap<String, Vec<FeedbackExample>> = HashMap::new();
    let mut explicit = HashMap::new();
    for feedback in stored {
        let label = if feedback.label > 0 {
            FeedbackLabel::Belongs
        } else {
            FeedbackLabel::DoesNotBelong
        };
        let weight = if feedback.source == "user" { 1.0 } else { 0.35 };
        let examples = examples_by_category
            .entry(feedback.category_id.clone())
            .or_default();
        if examples.len() < 512 {
            examples.push(FeedbackExample {
                values: feedback.embedding.values().to_vec(),
                label,
                weight,
            });
        }
        if feedback.source == "user" {
            explicit.insert(
                (
                    feedback.category_id,
                    feedback.photo_id,
                    feedback.representation_id,
                ),
                label,
            );
        }
    }
    let models = definitions
        .iter()
        .zip(category_embeddings)
        .map(|(definition, embedding)| {
            AdaptiveCategoryModel::new(
                embedding.values(),
                definition.minimum_similarity,
                examples_by_category
                    .remove(&definition.id)
                    .unwrap_or_default(),
            )
        })
        .collect();
    (models, explicit)
}

fn validate_request(
    definitions: &[SmartCategoryDefinition],
    policy: SmartClassificationPolicy,
) -> Result<(), SmartClassificationError> {
    if policy.batch_size == 0 || policy.batch_size > MAX_SMART_CLASSIFICATION_BATCH_SIZE {
        return Err(SmartClassificationError::InvalidPolicy);
    }
    if definitions.is_empty() || definitions.len() > MAX_SMART_CATEGORY_DEFINITIONS {
        return Err(SmartClassificationError::InvalidDefinitions);
    }
    for definition in definitions {
        let valid_id = !definition.id.is_empty()
            && definition.id.len() <= 80
            && definition
                .id
                .bytes()
                .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-');
        if !valid_id
            || definition.description.trim().is_empty()
            || definition.description.len() > 1024
            || !definition.minimum_similarity.is_finite()
            || !(-1.0..=1.0).contains(&definition.minimum_similarity)
        {
            return Err(SmartClassificationError::InvalidDefinitions);
        }
    }
    Ok(())
}

fn validate_config_revision(revision: &str) -> Result<(), SmartClassificationError> {
    if revision.is_empty()
        || revision.len() > 256
        || !revision.bytes().all(|byte| byte.is_ascii_graphic())
    {
        return Err(SmartClassificationError::InvalidRequest);
    }
    Ok(())
}

fn validate_category_id(category_id: &str) -> Result<(), SmartClassificationError> {
    let valid = !category_id.is_empty()
        && category_id.len() <= 80
        && category_id
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-');
    if valid {
        Ok(())
    } else {
        Err(SmartClassificationError::InvalidRequest)
    }
}

fn parse_status(status: &str) -> Result<SmartClassificationStatus, SmartClassificationError> {
    match status {
        "running" => Ok(SmartClassificationStatus::Running),
        "paused" => Ok(SmartClassificationStatus::Paused),
        "complete" => Ok(SmartClassificationStatus::Complete),
        "failed" => Ok(SmartClassificationStatus::Failed),
        _ => Err(SmartClassificationError::InvalidCheckpointStatus),
    }
}

fn category_revision(definition: &SmartCategoryDefinition) -> String {
    let mut hasher = Hasher::new();
    hasher.update(definition.id.as_bytes());
    hasher.update(&[0]);
    hasher.update(definition.description.as_bytes());
    format!("shadow:smart-category/query:{}", hasher.finalize().to_hex())
}

fn semantic_source_revision(photo_id: PhotoId, record: &CachedArtifactRecord) -> String {
    let mut digest = String::with_capacity(64);
    for byte in record.artifact.blob_digest {
        write!(&mut digest, "{byte:02x}").expect("writing into String cannot fail");
    }
    format!(
        "shadow:{photo_id}/representation:{}/artifact:{digest}",
        record.representation_id
    )
}

#[derive(Debug, Error)]
pub enum SmartClassificationError {
    #[error("smart classification policy is invalid")]
    InvalidPolicy,
    #[error("smart category definitions are invalid")]
    InvalidDefinitions,
    #[error("smart classification request is invalid")]
    InvalidRequest,
    #[error("smart classification checkpoint status is invalid")]
    InvalidCheckpointStatus,
    #[error("the semantic embedding space changed during classification")]
    EmbeddingSpaceChanged,
    #[error("infer-runtime image geometry disagrees with the selected Catalog artifact")]
    ProviderGeometryMismatch,
    #[error(transparent)]
    Catalog(#[from] CatalogError),
    #[error(transparent)]
    Cache(#[from] CachedArtifactLoadError),
    #[error(transparent)]
    Provider(#[from] InferRuntimeClientError),
    #[error(transparent)]
    Index(#[from] SemanticIndexError),
}

#[cfg(test)]
mod tests;
