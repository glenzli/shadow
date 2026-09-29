//! Transient selected-photo similarity suggestions for Review.

use super::*;
use shadow_ai::SemanticEmbedding;
use shadow_catalog::ReviewItemRecord;

const SIMILAR_REVIEW_NEIGHBORS: usize = 24;

/// Suggests a temporary, bounded comparison group around an exact Review photo.
///
/// A bounded path-order neighborhood supplies candidates. When both timestamps
/// exist, candidates more than two minutes apart are excluded. Similarity
/// orders candidates; it is not an aesthetic score or a calibrated duplicate
/// verdict. No Catalog decision is written.
///
/// # Errors
/// Returns cancellation, Catalog/cache-integrity, provider, or geometry failures.
pub fn suggest_similar_review_photos_with_control(
    catalog: &CatalogHandle,
    cache_root: impl Into<std::path::PathBuf>,
    provider: &impl SemanticEmbeddingProvider,
    anchor_photo_id: PhotoId,
    anchor_representation_id: RepresentationId,
    cancelled: &dyn Fn() -> bool,
) -> Result<SemanticSearchReport, SemanticSearchError> {
    let cache_root = cache_root.into();
    let embeddings = embedding_cache::EmbeddingCache::new(&cache_root);
    let loader = CachedArtifactLoader::open(catalog.clone(), cache_root)?;
    let mut cursor: Option<ReviewCursor> = None;
    let mut items = Vec::new();
    let mut truncated = false;
    let mut scanned_photos = 0;
    let mut anchor_found = false;
    let mut photos_after_anchor = 0;
    'pages: loop {
        if cancelled() {
            return Err(SemanticSearchError::Cancelled);
        }
        let page = catalog.review_page(cursor.as_ref(), REVIEW_PAGE_SIZE)?;
        if page.items.is_empty() {
            break;
        }
        for item in page.items {
            if scanned_photos == MAX_SEMANTIC_SEARCH_PHOTOS {
                truncated = true;
                break 'pages;
            }
            scanned_photos += 1;
            if anchor_found {
                if photos_after_anchor == SIMILAR_REVIEW_NEIGHBORS {
                    break 'pages;
                }
                photos_after_anchor += 1;
                items.push(item);
            } else if item.photo_id == anchor_photo_id
                && item.representation_id == anchor_representation_id
            {
                anchor_found = true;
                items.push(item);
            } else {
                if items.len() == SIMILAR_REVIEW_NEIGHBORS {
                    items.remove(0);
                }
                items.push(item);
            }
        }
        cursor = page.next_cursor;
        if cursor.is_none() {
            break;
        }
    }
    let anchor_index = items
        .iter()
        .position(|item| {
            item.photo_id == anchor_photo_id && item.representation_id == anchor_representation_id
        })
        .ok_or(SemanticSearchError::AnchorUnavailable)?;
    let anchor = &items[anchor_index];
    let anchor_record = anchor
        .visual
        .as_ref()
        .filter(|record| record.artifact.codec == PreviewCodec::Jpeg)
        .ok_or(SemanticSearchError::AnchorUnavailable)?;
    let anchor_revision = format!(
        "{}/input:jpeg2048q90v1",
        semantic_source_revision(anchor.photo_id, anchor_record)
    );
    let anchor_bytes = loader.load_bytes(anchor_record)?;
    let prepared =
        crate::vision_input::bounded_jpeg(&anchor_bytes, anchor_record.artifact.dimensions, 2048)
            .map_err(SemanticSearchError::InputPreparation)?;
    let anchor_evidence = provider
        .embed_image_semantics_cancellable(
            &prepared.bytes,
            "image/jpeg",
            &anchor_revision,
            SemanticRequestPriority::Interactive,
            cancelled,
        )?
        .ok_or(SemanticSearchError::Cancelled)?;
    if anchor_evidence.width != prepared.dimensions.width
        || anchor_evidence.height != prepared.dimensions.height
    {
        return Err(SemanticSearchError::ProviderGeometryMismatch);
    }
    if catalog.preferred_cached_artifact(anchor.representation_id)? != Some(anchor_record.clone()) {
        return Err(SemanticSearchError::AnchorUnavailable);
    }
    let anchor_embedding = anchor_evidence.embedding;
    let embedding_space = anchor_embedding.space().space_id().to_owned();
    let query_model_build = anchor_evidence.provenance.model_build;
    let anchor_time = capture_time(anchor);
    let mut skipped = SemanticSearchSkipped::default();
    let mut matches = vec![SemanticSearchMatch {
        photo_id: anchor.photo_id,
        representation_id: anchor.representation_id,
        cosine_similarity: 1.0,
    }];
    let mut considered_photos = 1;
    for (index, item) in items.iter().enumerate() {
        if cancelled() {
            return Err(SemanticSearchError::Cancelled);
        }
        if index == anchor_index {
            continue;
        }
        if let (Some(left), Some(right)) = (anchor_time, capture_time(item)) {
            if left.abs_diff(right) > 120 {
                continue;
            }
        }
        considered_photos += 1;
        let Some(embedding) = image_embedding_for_review_item(
            catalog,
            &loader,
            &embeddings,
            provider,
            item,
            &anchor_embedding,
            &mut skipped,
            cancelled,
        )?
        else {
            continue;
        };
        let Some(similarity) = embedding.cosine_similarity(&anchor_embedding) else {
            skipped.incompatible_embedding_space += 1;
            continue;
        };
        matches.push(SemanticSearchMatch {
            photo_id: item.photo_id,
            representation_id: item.representation_id,
            cosine_similarity: similarity,
        });
    }
    matches[1..].sort_by(|left, right| right.cosine_similarity.total_cmp(&left.cosine_similarity));
    let embedded_photos = matches.len();
    matches.truncate(13);
    Ok(SemanticSearchReport {
        query_revision: anchor_revision,
        embedding_space,
        query_model_build,
        considered_photos,
        embedded_photos,
        truncated,
        skipped,
        matches,
    })
}

fn capture_time(item: &ReviewItemRecord) -> Option<i64> {
    item.metadata
        .as_ref()
        .map(|metadata| metadata.captured_at_unix_seconds)
        .filter(|seconds| *seconds > 0)
}

#[allow(clippy::too_many_arguments)]
fn image_embedding_for_review_item(
    catalog: &CatalogHandle,
    loader: &CachedArtifactLoader,
    embeddings: &embedding_cache::EmbeddingCache,
    provider: &impl SemanticEmbeddingProvider,
    item: &ReviewItemRecord,
    anchor_embedding: &SemanticEmbedding,
    skipped: &mut SemanticSearchSkipped,
    cancelled: &dyn Fn() -> bool,
) -> Result<Option<SemanticEmbedding>, SemanticSearchError> {
    let Some(record) = item.visual.as_ref() else {
        skipped.no_current_visual += 1;
        return Ok(None);
    };
    if record.artifact.codec != PreviewCodec::Jpeg {
        skipped.unsupported_visual += 1;
        return Ok(None);
    }
    let source_revision = format!(
        "{}/input:jpeg2048q90v1",
        semantic_source_revision(item.photo_id, record)
    );
    let dimensions = record.artifact.dimensions;
    let embedding = if let Some(cached) = embeddings.load(
        &source_revision,
        anchor_embedding.space(),
        dimensions.width,
        dimensions.height,
    ) {
        cached
    } else {
        let image = loader.load_bytes(record)?;
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
        if !cancelled() && evidence.embedding.space() == anchor_embedding.space() {
            embeddings.store(
                &source_revision,
                &evidence.embedding,
                dimensions.width,
                dimensions.height,
            );
        }
        evidence.embedding
    };
    if catalog.preferred_cached_artifact(item.representation_id)? != Some(record.clone()) {
        skipped.stale_input += 1;
        return Ok(None);
    }
    Ok(Some(embedding))
}
