//! Bounded, session-only semantic search over current Review visuals.
//!
//! The Catalog selects current artifacts, the cache verifies their bytes, and
//! a semantic provider supplies image/text vectors. This owner keeps no index:
//! it rejects stale results and returns only ranked photo references.

use std::fmt::Write as _;

use serde::Serialize;
use shadow_ai::{InferRuntimeClientError, SemanticEmbeddingProvider, SemanticRequestPriority};
use shadow_catalog::{CachedArtifactRecord, CatalogError, CatalogHandle, ReviewCursor};
use shadow_domain::{PhotoId, PreviewCodec, RepresentationId};
use thiserror::Error;

use crate::{CachedArtifactLoadError, CachedArtifactLoader};

pub const DEFAULT_SEMANTIC_SEARCH_MAXIMUM_PHOTOS: usize = 32;
pub const MAX_SEMANTIC_SEARCH_PHOTOS: usize = 256;
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
/// This is deliberately a session preview, not a full-library indexing path.
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
    let policy = policy.validate()?;
    let query_evidence = provider.embed_text_semantics(
        query,
        query_revision,
        language,
        SemanticRequestPriority::Interactive,
    )?;
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
            let image = loader.load_bytes(&record)?;
            let source_revision = semantic_source_revision(item.photo_id, &record);
            let evidence = provider.embed_image_semantics(
                &image,
                "image/jpeg",
                &source_revision,
                SemanticRequestPriority::Background,
            )?;
            if evidence.width != record.artifact.dimensions.width
                || evidence.height != record.artifact.dimensions.height
            {
                return Err(SemanticSearchError::ProviderGeometryMismatch);
            }
            if catalog.preferred_cached_artifact(item.representation_id)? != Some(record) {
                skipped.stale_input += 1;
                continue;
            }
            let Some(similarity) = evidence
                .embedding
                .cosine_similarity(&query_evidence.embedding)
            else {
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
    #[error("semantic search policy is invalid")]
    InvalidPolicy,
    #[error("infer-runtime image geometry disagrees with the selected Catalog artifact")]
    ProviderGeometryMismatch,
    #[error(transparent)]
    Catalog(#[from] CatalogError),
    #[error(transparent)]
    Cache(#[from] CachedArtifactLoadError),
    #[error(transparent)]
    Provider(#[from] InferRuntimeClientError),
}

#[cfg(test)]
mod tests;
