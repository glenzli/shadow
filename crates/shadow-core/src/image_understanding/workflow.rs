use std::fmt::Write as _;

use shadow_ai::{
    ImageUnderstandingProvider, ImageUnderstandingQuality, InferRuntimeClientError,
    SemanticRequestPriority,
};
use shadow_catalog::{
    CachedArtifactRecord, CatalogError, CatalogHandle, LibraryKeywordAssignmentOrigin,
    LibraryPhotoFilter, LibraryPhotoOrder,
};
use shadow_domain::{PhotoId, PreviewCodec, RepresentationId};
use thiserror::Error;
use time::OffsetDateTime;

use crate::{CachedArtifactLoadError, CachedArtifactLoader};

use super::store::{ImageUnderstandingStore, StoreProposal};
use super::{
    ImageUnderstandingProposalDisposition, ImageUnderstandingRunSnapshot,
    ImageUnderstandingRunStatus, ImageUnderstandingScanPolicy, ImageUnderstandingStoreError,
};

const DEFAULT_ANALYSIS_LANGUAGE: &str = "zh-CN";

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ImageUnderstandingRequest {
    pub generation: Option<String>,
    pub start_new: bool,
    pub auto_apply_keywords: bool,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ImageUnderstandingBatch {
    pub snapshot: ImageUnderstandingRunSnapshot,
    pub analyzed_photos: usize,
    pub reused_photos: usize,
    pub skipped_photos: usize,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ImageUnderstandingKeywordAcceptance {
    Automatic,
    User,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ImageUnderstandingKeywordApplication {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub source_revision: String,
    pub requested_keywords: usize,
    pub changed_assignments: u64,
}

/// Projects the current resumable background run without starting model work.
///
/// # Errors
///
/// Returns sidecar filesystem, schema, or database errors.
pub fn image_understanding_snapshot(
    cache_root: impl AsRef<std::path::Path>,
) -> Result<Option<ImageUnderstandingRunSnapshot>, ImageUnderstandingStoreError> {
    ImageUnderstandingStore::open(cache_root)?.snapshot()
}

/// Pauses one exact generation so a later call can resume it safely.
///
/// # Errors
///
/// Returns stale-generation or sidecar persistence errors.
pub fn pause_image_understanding(
    cache_root: impl AsRef<std::path::Path>,
    generation: &str,
) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingStoreError> {
    ImageUnderstandingStore::open(cache_root)?.pause_run(generation)
}

/// Loads one rebuildable proposal for the exact logical photo representation.
///
/// # Errors
///
/// Returns sidecar filesystem, schema, serialization, or database errors.
pub fn image_understanding_proposal(
    cache_root: impl AsRef<std::path::Path>,
    photo_id: PhotoId,
    representation_id: RepresentationId,
) -> Result<Option<super::ImageUnderstandingProposal>, ImageUnderstandingStoreError> {
    ImageUnderstandingStore::open(cache_root)?.proposal(photo_id, representation_id)
}

/// Commits a proposal's keyword suggestions with explicit AI provenance.
///
/// Existing manual or imported assignments win over the model. Concept
/// identities remain open-ended in the rebuildable sidecar; Shadow creates or
/// reuses only an exact top-level display label instead of imposing a global
/// closed vocabulary.
///
/// # Errors
///
/// Returns stale-proposal, Catalog mutation, or sidecar errors. A partial
/// Catalog application is safe to retry because assignments and bindings are
/// idempotent.
pub fn apply_image_understanding_keywords(
    catalog: &CatalogHandle,
    cache_root: impl AsRef<std::path::Path>,
    photo_id: PhotoId,
    representation_id: RepresentationId,
    expected_source_revision: &str,
    acceptance: ImageUnderstandingKeywordAcceptance,
) -> Result<ImageUnderstandingKeywordApplication, ImageUnderstandingWorkflowError> {
    let store = ImageUnderstandingStore::open(cache_root)?;
    let proposal = store
        .proposal(photo_id, representation_id)?
        .ok_or(ImageUnderstandingWorkflowError::ProposalNotFound)?;
    if proposal.source_revision != expected_source_revision {
        return Err(ImageUnderstandingWorkflowError::StaleProposal);
    }
    let mut keyword_tree = catalog.library_keyword_tree()?;
    let mut changed_assignments = 0_u64;
    let source_label = format!(
        "{}:{}",
        proposal.provenance.model_build, proposal.analysis.prompt_revision
    );
    for suggestion in &proposal.analysis.suggestions {
        let bound = store.keyword_binding(&suggestion.concept_id)?;
        let keyword_id = bound
            .filter(|keyword_id| keyword_tree.iter().any(|item| item.id == *keyword_id))
            .or_else(|| {
                keyword_tree
                    .iter()
                    .find(|item| {
                        item.parent_id.is_none()
                            && item.name.trim().to_lowercase()
                                == suggestion.display_label.trim().to_lowercase()
                    })
                    .map(|item| item.id)
            })
            .map_or_else(
                || {
                    catalog
                        .create_library_keyword(None, &suggestion.display_label, now_ms())
                        .map(|created| {
                            let keyword_id = created.id;
                            keyword_tree.push(created);
                            keyword_id
                        })
                },
                Ok,
            )?;
        store.bind_keyword(
            &suggestion.concept_id,
            keyword_id,
            &suggestion.display_label,
        )?;
        let receipt = catalog.assign_library_keyword_to_photos(
            keyword_id,
            &[photo_id],
            LibraryKeywordAssignmentOrigin::AiAccepted,
            &source_label,
            None,
            now_ms(),
        )?;
        changed_assignments = changed_assignments.saturating_add(receipt.changed_photo_count);
    }
    let disposition = match acceptance {
        ImageUnderstandingKeywordAcceptance::Automatic => {
            ImageUnderstandingProposalDisposition::AutoApplied
        }
        ImageUnderstandingKeywordAcceptance::User => {
            ImageUnderstandingProposalDisposition::Accepted
        }
    };
    if !store.set_proposal_disposition(
        photo_id,
        representation_id,
        expected_source_revision,
        disposition,
    )? {
        return Err(ImageUnderstandingWorkflowError::StaleProposal);
    }
    Ok(ImageUnderstandingKeywordApplication {
        photo_id,
        representation_id,
        source_revision: expected_source_revision.into(),
        requested_keywords: proposal.analysis.suggestions.len(),
        changed_assignments,
    })
}

/// Processes at most one small Library page with the bulk structured model.
///
/// A call owns no long-running worker. The desktop scheduler invokes this
/// repeatedly at background priority; every page checkpoint and proposal is
/// durable, so process exit merely pauses progress. Explicit single-photo
/// general review is a separate admission path and does not broaden this scan.
///
/// # Errors
///
/// Returns policy, Catalog, cache-integrity, provider, geometry, or durable
/// checkpoint errors. The active generation is marked failed and can be
/// resumed explicitly with the same policy.
pub fn process_image_understanding_batch(
    catalog: &CatalogHandle,
    cache_root: impl AsRef<std::path::Path>,
    provider: &impl ImageUnderstandingProvider,
    policy: ImageUnderstandingScanPolicy,
    request: &ImageUnderstandingRequest,
) -> Result<ImageUnderstandingBatch, ImageUnderstandingWorkflowError> {
    let cache_root = cache_root.as_ref();
    let mut store = ImageUnderstandingStore::open(cache_root)?;
    let snapshot = prepare_run(catalog, &mut store, policy, request)?;
    if snapshot.status == ImageUnderstandingRunStatus::Complete {
        return Ok(ImageUnderstandingBatch {
            snapshot,
            analyzed_photos: 0,
            reused_photos: 0,
            skipped_photos: 0,
        });
    }
    let generation = snapshot.generation.clone();
    let result = process_prepared_batch(
        catalog,
        cache_root,
        provider,
        policy,
        &mut store,
        &generation,
        request.auto_apply_keywords,
    );
    if result.is_err() {
        let _ = store.fail_run(&generation);
    }
    result
}

fn prepare_run(
    catalog: &CatalogHandle,
    store: &mut ImageUnderstandingStore,
    policy: ImageUnderstandingScanPolicy,
    request: &ImageUnderstandingRequest,
) -> Result<ImageUnderstandingRunSnapshot, ImageUnderstandingWorkflowError> {
    if request.start_new {
        if request.generation.is_some() {
            return Err(ImageUnderstandingWorkflowError::InvalidRequest);
        }
        return Ok(store.start_run(policy, exact_total(catalog, policy)?)?);
    }
    let generation = request
        .generation
        .as_deref()
        .ok_or(ImageUnderstandingWorkflowError::InvalidRequest)?;
    Ok(store.resume_run(policy, generation)?)
}

fn process_prepared_batch(
    catalog: &CatalogHandle,
    cache_root: &std::path::Path,
    provider: &impl ImageUnderstandingProvider,
    policy: ImageUnderstandingScanPolicy,
    store: &mut ImageUnderstandingStore,
    generation: &str,
    auto_apply_keywords: bool,
) -> Result<ImageUnderstandingBatch, ImageUnderstandingWorkflowError> {
    let filters = policy.catalog_filters();
    let Some(branch_index) = (0..filters.len())
        .find(|index| !store.branch_complete(generation, *index).unwrap_or(false))
    else {
        return Err(ImageUnderstandingWorkflowError::InvalidCheckpoint);
    };
    let cursor = store.branch_cursor(generation, branch_index)?;
    let page = catalog.library_photo_page(
        &filters[branch_index],
        LibraryPhotoOrder::CaptureTimeAscending,
        cursor.as_ref(),
        policy.batch_size(),
    )?;
    let representation_ids = page
        .items
        .iter()
        .map(|item| item.representation_id)
        .collect::<Vec<_>>();
    let artifacts = catalog.preferred_cached_artifacts(&representation_ids)?;
    if artifacts.len() != page.items.len() {
        return Err(ImageUnderstandingWorkflowError::InvalidCheckpoint);
    }
    let loader = CachedArtifactLoader::open(catalog.clone(), cache_root)?;
    let mut analyzed_photos = 0;
    let mut reused_photos = 0;
    let mut skipped_photos = 0;

    for (item, artifact) in page.items.iter().zip(artifacts) {
        let Some(record) = artifact else {
            skipped_photos += 1;
            continue;
        };
        if record.artifact.codec != PreviewCodec::Jpeg {
            skipped_photos += 1;
            continue;
        }
        let expected_revision = source_revision(item.photo_id, &record);
        if store.was_processed(
            generation,
            item.photo_id,
            item.representation_id,
            &expected_revision,
        )? {
            reused_photos += 1;
            continue;
        }
        let bytes = loader.load_bytes(&record)?;
        let evidence = provider.describe_image(
            &bytes,
            "image/jpeg",
            &expected_revision,
            DEFAULT_ANALYSIS_LANGUAGE,
            ImageUnderstandingQuality::Basic,
            SemanticRequestPriority::Background,
        )?;
        if evidence.source_revision != expected_revision
            || evidence.width != record.artifact.dimensions.width
            || evidence.height != record.artifact.dimensions.height
            || evidence.orientation != "display_pixels_orientation_normalized"
        {
            return Err(ImageUnderstandingWorkflowError::ProviderGeometryMismatch);
        }
        let current = catalog.preferred_cached_artifact(item.representation_id)?;
        let current_revision = current
            .as_ref()
            .map(|current| source_revision(item.photo_id, current))
            .unwrap_or_default();
        store.store_proposal(&StoreProposal {
            generation,
            photo_id: item.photo_id,
            representation_id: item.representation_id,
            expected_source_revision: &expected_revision,
            current_source_revision: &current_revision,
            analysis: &evidence.analysis,
            provenance: &evidence.provenance,
            disposition: ImageUnderstandingProposalDisposition::Suggested,
        })?;
        apply_automatic_keywords_if_enabled(
            catalog,
            cache_root,
            item.photo_id,
            item.representation_id,
            &expected_revision,
            auto_apply_keywords,
        )?;
        analyzed_photos += 1;
    }

    let complete = page.next_cursor.is_none();
    let snapshot = store.checkpoint_branch(
        generation,
        branch_index,
        page.next_cursor.as_ref(),
        complete,
    )?;
    Ok(ImageUnderstandingBatch {
        snapshot,
        analyzed_photos,
        reused_photos,
        skipped_photos,
    })
}

fn apply_automatic_keywords_if_enabled(
    catalog: &CatalogHandle,
    cache_root: &std::path::Path,
    photo_id: PhotoId,
    representation_id: RepresentationId,
    expected_source_revision: &str,
    enabled: bool,
) -> Result<(), ImageUnderstandingWorkflowError> {
    if enabled {
        apply_image_understanding_keywords(
            catalog,
            cache_root,
            photo_id,
            representation_id,
            expected_source_revision,
            ImageUnderstandingKeywordAcceptance::Automatic,
        )?;
    }
    Ok(())
}

fn exact_total(
    catalog: &CatalogHandle,
    policy: ImageUnderstandingScanPolicy,
) -> Result<u64, CatalogError> {
    let filters = policy.catalog_filters();
    if filters.len() == 1 {
        return catalog.library_photo_count(&filters[0]);
    }
    let liked = catalog.library_photo_count(&filters[0])?;
    let rated = catalog.library_photo_count(&filters[1])?;
    let intersection = catalog.library_photo_count(&LibraryPhotoFilter {
        liked: Some(true),
        minimum_rating: policy.minimum_rating(),
        ..LibraryPhotoFilter::default()
    })?;
    Ok(liked.saturating_add(rated).saturating_sub(intersection))
}

pub(super) fn source_revision(photo_id: PhotoId, record: &CachedArtifactRecord) -> String {
    let mut digest = String::with_capacity(64);
    for byte in record.artifact.blob_digest {
        write!(&mut digest, "{byte:02x}").expect("writing into String cannot fail");
    }
    format!(
        "shadow:{photo_id}/representation:{}/artifact:{digest}",
        record.representation_id
    )
}

fn now_ms() -> i64 {
    let milliseconds = OffsetDateTime::now_utc().unix_timestamp_nanos() / 1_000_000;
    i64::try_from(milliseconds).unwrap_or(i64::MAX)
}

#[derive(Debug, Error)]
pub enum ImageUnderstandingWorkflowError {
    #[error("image-understanding request is invalid")]
    InvalidRequest,
    #[error("image-understanding checkpoint is invalid")]
    InvalidCheckpoint,
    #[error("image-understanding proposal was not found")]
    ProposalNotFound,
    #[error("image-understanding proposal belongs to a stale source revision")]
    StaleProposal,
    #[error("image-understanding provider geometry disagrees with the current display artifact")]
    ProviderGeometryMismatch,
    #[error(transparent)]
    Catalog(#[from] CatalogError),
    #[error(transparent)]
    Cache(#[from] CachedArtifactLoadError),
    #[error(transparent)]
    Provider(#[from] InferRuntimeClientError),
    #[error(transparent)]
    Store(#[from] ImageUnderstandingStoreError),
}
