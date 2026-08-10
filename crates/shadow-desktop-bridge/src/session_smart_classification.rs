//! Desktop projection for resumable smart-category classification.

use std::path::Path;

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_ai::InferRuntimeClient;
use shadow_core::{
    SmartCategoryDefinition, SmartCategoryFeedbackDecision, SmartClassificationPolicy,
    SmartClassificationRequest, SmartClassificationSnapshot, SmartClassificationStatus,
    classify_review_smart_categories, pause_smart_classification, set_smart_category_feedback,
    smart_category_members, smart_category_review_queue, smart_classification_snapshot,
};
use shadow_domain::{PhotoId, RepresentationId};

use super::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn classify_smart_categories_batch(
        &self,
        infer_base_url: &str,
        credential_file: &str,
        definitions: Vec<ffi::FfiSmartCategoryDefinition>,
        config_revision: &str,
        generation: &str,
        start_new: bool,
        clear_embeddings: bool,
    ) -> AnyResult<ffi::FfiSmartClassificationBatch> {
        let provider =
            InferRuntimeClient::from_credential_file(infer_base_url, Path::new(credential_file))
                .context("configure local smart-category provider")?;
        let definitions = definitions
            .into_iter()
            .map(|definition| SmartCategoryDefinition {
                id: definition.id,
                description: definition.description,
                minimum_similarity: definition.minimum_similarity,
            })
            .collect::<Vec<_>>();
        let batch = classify_review_smart_categories(
            &self.catalog,
            &self.cache_root,
            &provider,
            &definitions,
            &SmartClassificationRequest {
                config_revision: config_revision.into(),
                generation: (!generation.is_empty()).then(|| generation.into()),
                start_new,
                clear_embeddings,
            },
            SmartClassificationPolicy::default(),
        )
        .map_err(|error| anyhow!("classify current Library visuals: {error}"))?;
        Ok(ffi::FfiSmartClassificationBatch {
            config_revision: batch.config_revision,
            generation: batch.generation,
            status: status_code(batch.status).into(),
            embedding_space: batch.embedding_space,
            model_build: batch.model_build,
            processed_photos: batch.processed_photos,
            embedded_photos: bounded_u32(batch.embedded_photos, "embedded photo count")?,
            reused_photos: bounded_u32(batch.reused_photos, "reused photo count")?,
            skipped_photos: bounded_u32(batch.skipped_photos, "skipped photo count")?,
            total_photos: batch.total_photos,
        })
    }

    pub(crate) fn smart_classification_snapshot(
        &self,
    ) -> AnyResult<ffi::FfiSmartClassificationSnapshot> {
        ffi_snapshot(
            smart_classification_snapshot(&self.cache_root)
                .map_err(|error| anyhow!("load smart-category checkpoint: {error}"))?,
        )
    }

    pub(crate) fn smart_category_members(&self, category_id: &str) -> AnyResult<Vec<String>> {
        smart_category_members(&self.cache_root, category_id)
            .map_err(|error| anyhow!("load smart-category members: {error}"))
    }

    pub(crate) fn smart_category_review_queue(
        &self,
    ) -> AnyResult<Vec<ffi::FfiSmartCategoryReviewItem>> {
        smart_category_review_queue(&self.cache_root)
            .map_err(|error| anyhow!("load smart-category review queue: {error}"))
            .map(|items| {
                items
                    .into_iter()
                    .map(|item| ffi::FfiSmartCategoryReviewItem {
                        photo_id: item.photo_id.to_string(),
                        representation_id: item.representation_id.to_string(),
                        category_id: item.category_id,
                        adapted_similarity: item.adapted_similarity,
                        decision_margin: item.decision_margin,
                    })
                    .collect()
            })
    }

    pub(crate) fn set_smart_category_feedback(
        &self,
        photo_id: &str,
        representation_id: &str,
        category_id: &str,
        decision: i8,
    ) -> AnyResult<()> {
        let photo_id = photo_id
            .parse::<PhotoId>()
            .context("parse smart-category feedback photo identity")?;
        let representation_id = representation_id
            .parse::<RepresentationId>()
            .context("parse smart-category feedback representation identity")?;
        let decision = match decision {
            -1 => SmartCategoryFeedbackDecision::DoesNotBelong,
            0 => SmartCategoryFeedbackDecision::Clear,
            1 => SmartCategoryFeedbackDecision::Belongs,
            _ => return Err(anyhow!("smart-category feedback decision is invalid")),
        };
        set_smart_category_feedback(
            &self.cache_root,
            photo_id,
            representation_id,
            category_id,
            decision,
        )
        .map_err(|error| anyhow!("record smart-category feedback: {error}"))
    }

    pub(crate) fn pause_smart_classification(&self, generation: &str) -> AnyResult<()> {
        pause_smart_classification(&self.cache_root, generation)
            .map_err(|error| anyhow!("pause smart-category classification: {error}"))
    }
}

fn ffi_snapshot(
    snapshot: SmartClassificationSnapshot,
) -> AnyResult<ffi::FfiSmartClassificationSnapshot> {
    Ok(ffi::FfiSmartClassificationSnapshot {
        config_revision: snapshot.config_revision,
        generation: snapshot.generation,
        status: status_code(snapshot.status).into(),
        embedding_space: snapshot.embedding_space,
        model_build: snapshot.model_build,
        processed_photos: snapshot.processed_photos,
        total_photos: snapshot.total_photos,
        updated_at_ms: snapshot.updated_at_ms,
        has_published_results: snapshot.has_published_results,
        published_config_revision: snapshot.published_config_revision,
        published_at_ms: snapshot.published_at_ms,
        category_counts: snapshot
            .category_counts
            .into_iter()
            .map(|count| ffi::FfiSmartCategoryCount {
                category_id: count.category_id,
                count: count.count,
            })
            .collect(),
        uncertain_photos: snapshot.uncertain_photos,
        adaptation_pending: snapshot.adaptation_pending,
    })
}

fn status_code(status: SmartClassificationStatus) -> &'static str {
    match status {
        SmartClassificationStatus::Empty => "empty",
        SmartClassificationStatus::Running => "running",
        SmartClassificationStatus::Paused => "paused",
        SmartClassificationStatus::Complete => "complete",
        SmartClassificationStatus::Failed => "failed",
    }
}

fn bounded_u32(value: usize, field: &'static str) -> AnyResult<u32> {
    u32::try_from(value).with_context(|| format!("{field} exceeds the desktop ABI bound"))
}
