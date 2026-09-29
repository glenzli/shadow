//! Cancellable semantic search over current Review visuals with rebuildable image vectors.
//!
//! The Catalog selects current artifacts, the cache verifies their bytes, and
//! a semantic provider supplies image/text vectors. Exact image vectors are cached
//! independently of queries; stale results are rejected before ranked references return.

mod embedding_cache;

use std::fmt::Write as _;

use serde::Serialize;
use shadow_ai::{InferRuntimeClientError, SemanticEmbeddingProvider, SemanticRequestPriority};
use shadow_catalog::{CachedArtifactRecord, CatalogError, CatalogHandle, ReviewCursor};
use shadow_domain::{PhotoId, PreviewCodec, RepresentationId};
use thiserror::Error;

use crate::{CachedArtifactLoadError, CachedArtifactLoader};

pub const DEFAULT_SEMANTIC_SEARCH_MAXIMUM_PHOTOS: usize = 32;
pub const MAX_SEMANTIC_SEARCH_PHOTOS: usize = 100_000;
const REVIEW_PAGE_SIZE: usize = 64;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct SemanticSearchPolicy {
    pub maximum_photos: usize,
}

impl Default for SemanticSearchPolicy {
    fn default() -> Self {
        Self {
            maximum_photos: DEFAULT_SEMANTIC_SEARCH_MAXIMUM_PHOTOS,
        }
    }
}

impl SemanticSearchPolicy {
    fn validate(self) -> Result<Self, SemanticSearchError> {
        if self.maximum_photos == 0 || self.maximum_photos > MAX_SEMANTIC_SEARCH_PHOTOS {
            return Err(SemanticSearchError::InvalidPolicy);
        }
        Ok(self)
    }
}

#[derive(Debug, Default, Copy, Clone, Eq, PartialEq, Serialize)]
pub struct SemanticSearchSkipped {
    pub no_current_visual: usize,
    pub unsupported_visual: usize,
    pub stale_input: usize,
    pub incompatible_embedding_space: usize,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SemanticSearchMatch {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub cosine_similarity: f32,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SemanticSearchReport {
    pub query_revision: String,
    pub embedding_space: String,
    pub query_model_build: String,
    pub considered_photos: usize,
    pub embedded_photos: usize,
    pub truncated: bool,
    pub skipped: SemanticSearchSkipped,
    pub matches: Vec<SemanticSearchMatch>,
}

/// Ranks a bounded prefix of current Review visuals against one text query.
///
/// The caller controls coverage; image vectors are reusable across queries.
/// Image requests use background priority; the single query request is
/// interactive. Every image result is discarded if its exact Catalog artifact
/// changed while inference was running.
///
/// # Errors
///
/// Returns policy, Catalog/cache-integrity, provider, or geometry failures.
pub fn search_review_semantics(
    catalog: &CatalogHandle,
    cache_root: impl Into<std::path::PathBuf>,
    provider: &impl SemanticEmbeddingProvider,
    query: &str,
    query_revision: &str,
    language: Option<&str>,
    policy: SemanticSearchPolicy,
) -> Result<SemanticSearchReport, SemanticSearchError> {
    search_review_semantics_with_control(
        catalog,
        cache_root,
        provider,
        query,
        query_revision,
        language,
        policy,
        &|| false,
    )
}

/// Searches verified current visuals with reusable embeddings and in-flight cancellation.
/// # Errors
/// Returns cancellation, policy, Catalog, cache-integrity, or provider errors.
#[allow(clippy::too_many_arguments)]
pub fn search_review_semantics_with_control(
    catalog: &CatalogHandle,
    cache_root: impl Into<std::path::PathBuf>,
    provider: &impl SemanticEmbeddingProvider,
    query: &str,
    query_revision: &str,
    language: Option<&str>,
    policy: SemanticSearchPolicy,
    cancelled: &dyn Fn() -> bool,
) -> Result<SemanticSearchReport, SemanticSearchError> {
    let policy = policy.validate()?;
    if cancelled() {
        return Err(SemanticSearchError::Cancelled);
    }
    let cache_root = cache_root.into();
    let embeddings = embedding_cache::EmbeddingCache::new(&cache_root);
    let query_evidence = provider
        .embed_text_semantics_cancellable(
            query,
            query_revision,
            language,
            SemanticRequestPriority::Interactive,
            cancelled,
        )?
        .ok_or(SemanticSearchError::Cancelled)?;
    let embedding_space = query_evidence.embedding.space().space_id().to_owned();
    let loader = CachedArtifactLoader::open(catalog.clone(), cache_root)?;
    let mut cursor: Option<ReviewCursor> = None;
    let mut considered_photos = 0;
    let mut skipped = SemanticSearchSkipped::default();
    let mut matches = Vec::new();
    let mut truncated = false;

    'pages: loop {
        let page = catalog.review_page(cursor.as_ref(), REVIEW_PAGE_SIZE)?;
        if page.items.is_empty() {
            break;
        }
        for item in page.items {
            if cancelled() {
                return Err(SemanticSearchError::Cancelled);
            }
            if considered_photos == policy.maximum_photos {
                truncated = true;
                break 'pages;
            }
            considered_photos += 1;
            let Some(record) = item.visual else {
                skipped.no_current_visual += 1;
                continue;
            };
            if record.artifact.codec != PreviewCodec::Jpeg {
                skipped.unsupported_visual += 1;
                continue;
            }
            let source_revision = format!(
                "{}/input:jpeg2048q90v1",
                semantic_source_revision(item.photo_id, &record)
            );
            let dimensions = record.artifact.dimensions;
            let embedding = if let Some(cached) = embeddings.load(
                &source_revision,
                query_evidence.embedding.space(),
                dimensions.width,
                dimensions.height,
            ) {
                cached
            } else {
                let image = loader.load_bytes(&record)?;
                let prepared = crate::vision_input::bounded_jpeg(&image, dimensions, 2048)
                    .map_err(SemanticSearchError::InputPreparation)?;
                let evidence = provider
                    .embed_image_semantics_cancellable(
                        &prepared.bytes,
                        "image/jpeg",
                        &source_revision,
                        SemanticRequestPriority::Background,
                        cancelled,
                    )?
                    .ok_or(SemanticSearchError::Cancelled)?;
                if evidence.width != prepared.dimensions.width
                    || evidence.height != prepared.dimensions.height
                {
                    return Err(SemanticSearchError::ProviderGeometryMismatch);
                }
                if !cancelled() && evidence.embedding.space() == query_evidence.embedding.space() {
                    embeddings.store(
                        &source_revision,
                        &evidence.embedding,
                        dimensions.width,
                        dimensions.height,
                    );
                }
                evidence.embedding
            };
            if catalog.preferred_cached_artifact(item.representation_id)? != Some(record) {
                skipped.stale_input += 1;
                continue;
            }
            let Some(similarity) = embedding.cosine_similarity(&query_evidence.embedding) else {
                skipped.incompatible_embedding_space += 1;
                continue;
            };
            matches.push(SemanticSearchMatch {
                photo_id: item.photo_id,
                representation_id: item.representation_id,
                cosine_similarity: similarity,
            });
        }
        cursor = page.next_cursor;
        if cursor.is_none() {
            break;
        }
    }

    if cancelled() {
        return Err(SemanticSearchError::Cancelled);
    }
    matches.sort_by(|left, right| right.cosine_similarity.total_cmp(&left.cosine_similarity));
    Ok(SemanticSearchReport {
        query_revision: query_evidence.query_revision,
        embedding_space,
        query_model_build: query_evidence.provenance.model_build,
        considered_photos,
        embedded_photos: matches.len(),
        truncated,
        skipped,
        matches,
    })
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
pub enum SemanticSearchError {
    #[error("semantic search cancelled")]
    Cancelled,
    #[error("semantic search policy is invalid")]
    InvalidPolicy,
    #[error("infer-runtime image geometry disagrees with the selected Catalog artifact")]
    ProviderGeometryMismatch,
    #[error("could not prepare local vision input: {0}")]
    InputPreparation(String),
    #[error(transparent)]
    Catalog(#[from] CatalogError),
    #[error(transparent)]
    Cache(#[from] CachedArtifactLoadError),
    #[error(transparent)]
    Provider(#[from] InferRuntimeClientError),
}

#[cfg(test)]
mod tests;
