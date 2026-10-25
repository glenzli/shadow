use super::*;
use shadow_catalog::ReviewCursor;
use shadow_domain::{EntityId, PhotoId, RepresentationId};

use shadow_ai::{
    ImageEmbeddingEvidence, SemanticEmbeddingSpace, TextEmbeddingEvidence, VisionProvenance,
};
use std::cell::Cell;

struct CancellingProvider {
    cancelled: Cell<bool>,
    cancel_text: Cell<bool>,
    cancel_image_call: Cell<Option<usize>>,
    image_calls: Cell<usize>,
}

fn classification_embedding() -> SemanticEmbedding {
    SemanticEmbedding::new(
        SemanticEmbeddingSpace::new(
            shadow_ai::SEMANTIC_EMBEDDING_CONTRACT_VERSION,
            "test-space",
            2,
        )
        .unwrap(),
        vec![1.0, 0.0],
    )
    .unwrap()
}

fn classification_provenance() -> VisionProvenance {
    VisionProvenance {
        job_id: "test-job".into(),
        provider: "test".into(),
        deployment: "test".into(),
        model_build: "test-build".into(),
        artifact_sha256: "test-sha".into(),
        preprocessing_identity: "test-preprocess".into(),
        postprocessing_identity: "test-postprocess".into(),
        tokenizer: None,
        runtime: "test-runtime".into(),
        requested_execution_provider: "cpu".into(),
        actual_execution_provider: "cpu".into(),
        execution_provider_fallback_reason: None,
        precision: "fp32".into(),
    }
}

impl SemanticEmbeddingProvider for CancellingProvider {
    fn embed_image_semantics(
        &self,
        _: &[u8],
        _: &str,
        revision: &str,
        _: SemanticRequestPriority,
    ) -> Result<ImageEmbeddingEvidence, InferRuntimeClientError> {
        self.image_calls.set(self.image_calls.get() + 1);
        Ok(ImageEmbeddingEvidence {
            source_revision: revision.into(),
            width: 2,
            height: 2,
            orientation: "display_pixels_orientation_normalized".into(),
            embedding: classification_embedding(),
            provenance: classification_provenance(),
        })
    }
    fn embed_text_semantics(
        &self,
        _: &str,
        revision: &str,
        language: Option<&str>,
        _: SemanticRequestPriority,
    ) -> Result<TextEmbeddingEvidence, InferRuntimeClientError> {
        Ok(TextEmbeddingEvidence {
            query_revision: revision.into(),
            language: language.map(str::to_owned),
            embedding: classification_embedding(),
            provenance: classification_provenance(),
        })
    }
    fn embed_text_semantics_cancellable(
        &self,
        text: &str,
        revision: &str,
        language: Option<&str>,
        priority: SemanticRequestPriority,
        _: &dyn Fn() -> bool,
    ) -> Result<Option<TextEmbeddingEvidence>, InferRuntimeClientError> {
        if self.cancel_text.get() {
            self.cancelled.set(true);
            return Ok(None);
        }
        self.embed_text_semantics(text, revision, language, priority)
            .map(Some)
    }
    fn embed_image_semantics_cancellable(
        &self,
        image: &[u8],
        media: &str,
        revision: &str,
        priority: SemanticRequestPriority,
        _: &dyn Fn() -> bool,
    ) -> Result<Option<ImageEmbeddingEvidence>, InferRuntimeClientError> {
        let value = self.embed_image_semantics(image, media, revision, priority)?;
        if self.cancel_image_call.get() == Some(self.image_calls.get()) {
            self.cancelled.set(true);
            return Ok(None);
        }
        Ok(Some(value))
    }
}

#[test]
fn cancelled_provider_calls_preserve_checkpoint_and_published_memberships_on_resume() {
    use shadow_cache::ContentAddressedStore;
    use shadow_catalog::{
        CachedArtifact, CachedArtifactRole, CatalogActor, RecordCachedArtifact, RegisterAsset,
        RepresentationFingerprint,
    };
    use shadow_domain::{
        AssetLocation, ImageDimensions, Platform, PreviewByteOrder, RepresentationKind,
    };
    let root = std::env::temp_dir().join(format!(
        "shadow-classification-resume-{}",
        PhotoId::new_v7()
    ));
    std::fs::create_dir_all(&root).unwrap();
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).unwrap();
    let catalog = actor.handle();
    let cache_root = root.join("cache");
    let store = ContentAddressedStore::open(&cache_root).unwrap();
    for i in 0..3_u8 {
        let source = RepresentationFingerprint {
            byte_len: 4096,
            modified_at_ms: Some(1),
        };
        let path = format!("/classification/{i}.dng");
        let photo = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    path.as_bytes().to_vec(),
                    path.clone(),
                ),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 1,
            })
            .unwrap();
        let blob = store.put(&[i]).unwrap();
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: photo.representation_id,
                expected_source: source,
                artifact: CachedArtifact {
                    role: CachedArtifactRole::GeneratedProxy,
                    variant_key: format!("test-{i}"),
                    generator_id: "test".into(),
                    generator_version: "1".into(),
                    recipe_snapshot_digest: None,
                    provider_preview_id: None,
                    blob_algorithm: blob.digest.algorithm().into(),
                    blob_digest: *blob.digest.as_bytes(),
                    blob_byte_len: blob.byte_len,
                    codec: PreviewCodec::Jpeg,
                    byte_order: PreviewByteOrder::NotApplicable,
                    dimensions: ImageDimensions {
                        width: 2,
                        height: 2,
                    },
                    bits_per_channel: 8,
                    channels: 3,
                    created_at_ms: 1,
                },
            })
            .unwrap();
    }
    let provider = CancellingProvider {
        cancelled: Cell::new(false),
        cancel_text: Cell::new(true),
        cancel_image_call: Cell::new(None),
        image_calls: Cell::new(0),
    };
    let definitions = [SmartCategoryDefinition {
        id: "portrait".into(),
        description: "portrait".into(),
        minimum_similarity: 0.2,
    }];
    let mut request = SmartClassificationRequest {
        config_revision: "config".into(),
        generation: None,
        start_new: true,
        clear_embeddings: false,
    };
    let run = |request: &SmartClassificationRequest, policy| {
        classify_review_smart_categories_cancellable(
            &catalog,
            &cache_root,
            &provider,
            &definitions,
            request,
            policy,
            &|| provider.cancelled.get(),
        )
        .unwrap()
    };
    let paused = run(&request, SmartClassificationPolicy::default());
    assert_eq!(paused.status, SmartClassificationStatus::Paused);
    assert_eq!(paused.processed_photos, 0);
    assert_eq!(provider.image_calls.get(), 0);
    provider.cancel_text.set(false);
    provider.cancelled.set(false);
    request.start_new = false;
    request.generation = Some(paused.generation);
    let complete = run(&request, SmartClassificationPolicy::default());
    assert_eq!(complete.status, SmartClassificationStatus::Complete);
    assert_eq!(
        smart_category_members(&cache_root, "portrait")
            .unwrap()
            .len(),
        3
    );
    request.start_new = true;
    request.clear_embeddings = true;
    request.generation = None;
    provider.image_calls.set(0);
    provider.cancel_image_call.set(Some(2));
    let paused = run(&request, SmartClassificationPolicy { batch_size: 2 });
    assert_eq!(paused.status, SmartClassificationStatus::Paused);
    let index = SemanticIndex::open(&cache_root).unwrap();
    let checkpoint = index.run_checkpoint().unwrap().unwrap();
    assert_eq!(checkpoint.status, "paused");
    assert_eq!(checkpoint.processed_photos, 0);
    assert!(
        checkpoint.cursor.is_none(),
        "an incomplete page is never skipped on resume"
    );
    assert_eq!(
        index.published_snapshot().unwrap().unwrap().generation,
        complete.generation
    );
    assert_eq!(
        smart_category_members(&cache_root, "portrait")
            .unwrap()
            .len(),
        3
    );
    provider.cancelled.set(false);
    provider.cancel_image_call.set(None);
    request.start_new = false;
    request.clear_embeddings = false;
    request.generation = Some(paused.generation);
    let resumed = run(&request, SmartClassificationPolicy { batch_size: 2 });
    assert_eq!(resumed.status, SmartClassificationStatus::Running);
    assert_eq!(resumed.processed_photos, 2);
    assert_eq!(
        resumed.reused_photos, 1,
        "verified work in the cancelled page remains reusable"
    );
    assert_eq!(
        run(&request, SmartClassificationPolicy { batch_size: 2 }).status,
        SmartClassificationStatus::Complete
    );
    drop(index);
    drop(store);
    actor.shutdown().unwrap();
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn definitions_are_open_ended_multi_label_prompts() {
    let definitions = vec![
        SmartCategoryDefinition {
            id: "portrait".into(),
            description: "a portrait photograph of a person".into(),
            minimum_similarity: 0.2,
        },
        SmartCategoryDefinition {
            id: "travel".into(),
            description: "a travel photograph showing a place or journey".into(),
            minimum_similarity: 0.2,
        },
    ];
    assert!(validate_request(&definitions, SmartClassificationPolicy::default()).is_ok());
    assert_ne!(
        category_revision(&definitions[0]),
        category_revision(&definitions[1])
    );
}

#[test]
fn invalid_or_unbounded_requests_fail_before_inference() {
    let invalid = [SmartCategoryDefinition {
        id: "Portrait / People".into(),
        description: "portrait".into(),
        minimum_similarity: 0.2,
    }];
    assert!(matches!(
        validate_request(&invalid, SmartClassificationPolicy::default()),
        Err(SmartClassificationError::InvalidDefinitions)
    ));
    assert!(matches!(
        validate_request(
            &[SmartCategoryDefinition {
                id: "portrait".into(),
                description: "portrait".into(),
                minimum_similarity: 0.2,
            }],
            SmartClassificationPolicy { batch_size: 0 }
        ),
        Err(SmartClassificationError::InvalidPolicy)
    ));
}

#[test]
fn category_matches_require_absolute_evidence_and_relative_competitiveness() {
    let definitions = [
        SmartCategoryDefinition {
            id: "landscape".into(),
            description: "landscape".into(),
            minimum_similarity: 0.07,
        },
        SmartCategoryDefinition {
            id: "architecture".into(),
            description: "architecture".into(),
            minimum_similarity: 0.06,
        },
        SmartCategoryDefinition {
            id: "travel".into(),
            description: "travel".into(),
            minimum_similarity: 0.07,
        },
        SmartCategoryDefinition {
            id: "night".into(),
            description: "night".into(),
            minimum_similarity: 0.07,
        },
    ];

    assert_eq!(
        competitive_category_indices(&definitions, &[0.089, 0.061, 0.083, 0.050]),
        vec![0, 2],
        "an absolute architecture score is rejected when stronger scene labels disagree"
    );
    assert_eq!(
        competitive_category_indices(&definitions, &[0.023, 0.084, 0.088, 0.093]),
        vec![1, 2, 3],
        "near-equal city, travel, and night evidence preserves intentional overlap"
    );
    assert!(
        competitive_category_indices(&definitions, &[0.028, 0.018, 0.025, 0.010]).is_empty(),
        "the best weak label does not force an uncategorized image into a category"
    );
}

#[test]
fn index_round_trips_vectors_without_debug_disclosure() {
    let root = std::env::temp_dir().join(format!(
        "shadow-smart-category-index-{}",
        shadow_domain::PhotoId::new_v7()
    ));
    let index = SemanticIndex::open(&root).expect("open index");
    let space = shadow_ai::SemanticEmbeddingSpace::new(
        shadow_ai::SEMANTIC_EMBEDDING_CONTRACT_VERSION,
        "siglip-test@build:space:v1",
        2,
    )
    .expect("space");
    let embedding = shadow_ai::SemanticEmbedding::new(space, vec![1.0, 0.0]).expect("embedding");
    index
        .store_text("shadow:test", &embedding, "test-build")
        .expect("store");
    let cached = index
        .text("shadow:test", "siglip-test@build:space:v1")
        .expect("load")
        .expect("cached");
    assert_eq!(cached.embedding.values(), &[1.0, 0.0]);
    assert!(!format!("{:?}", cached.embedding).contains("1.0"));
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn durable_checkpoint_resumes_and_publishes_atomically() {
    let root = std::env::temp_dir().join(format!(
        "shadow-smart-category-checkpoint-{}",
        PhotoId::new_v7()
    ));
    let mut index = SemanticIndex::open(&root).expect("open index");
    let first_photo = PhotoId::new_v7();
    let first_representation = RepresentationId::new_v7();
    let second_photo = PhotoId::new_v7();
    let second_representation = RepresentationId::new_v7();
    let cursor = ReviewCursor {
        display_path: "/stable/catalog/order".into(),
        representation_id: first_representation,
    };
    let run = index
        .prepare_run("config-v1", None, true, false)
        .expect("start run");
    let checkpoint = index
        .checkpoint_batch(
            &run,
            "siglip-test@build:space:v1",
            "test-build",
            1,
            2,
            Some(&cursor),
            &[SmartCategoryMatch {
                photo_id: first_photo,
                representation_id: first_representation,
                source_revision: "source-a".into(),
                category_id: "portrait".into(),
                cosine_similarity: 0.8,
            }],
            &[SmartClassifiedPhoto {
                photo_id: first_photo,
                representation_id: first_representation,
                source_revision: "source-a".into(),
            }],
            &[],
        )
        .expect("checkpoint first batch");
    assert_eq!(checkpoint.processed_photos, 1);
    assert_eq!(checkpoint.cursor, Some(cursor.clone()));
    assert!(
        index
            .published_snapshot()
            .expect("published query")
            .is_none()
    );
    assert!(matches!(
        index.prepare_run("config-v1", Some("another-generation"), false, false),
        Err(SemanticIndexError::InvalidCheckpoint)
    ));

    index.mark_paused(&run.generation).expect("pause");
    let resumed = index
        .prepare_run("config-v1", Some(&run.generation), false, false)
        .expect("resume");
    assert_eq!(resumed.generation, run.generation);
    assert_eq!(resumed.processed_photos, 1);
    assert_eq!(resumed.cursor, Some(cursor));
    index
        .checkpoint_batch(
            &resumed,
            "siglip-test@build:space:v1",
            "test-build",
            1,
            2,
            None,
            &[SmartCategoryMatch {
                photo_id: second_photo,
                representation_id: second_representation,
                source_revision: "source-b".into(),
                category_id: "portrait".into(),
                cosine_similarity: 0.7,
            }],
            &[SmartClassifiedPhoto {
                photo_id: second_photo,
                representation_id: second_representation,
                source_revision: "source-b".into(),
            }],
            &[SmartCategoryUncertainty {
                photo_id: second_photo,
                representation_id: second_representation,
                source_revision: "source-b".into(),
                category_id: "animals".into(),
                adapted_similarity: 0.061,
                decision_margin: 0.001,
            }],
        )
        .expect("publish final batch");
    let published = index
        .published_snapshot()
        .expect("published query")
        .expect("published generation");
    assert_eq!(published.config_revision, "config-v1");
    assert_eq!(published.total_photos, 2);
    assert_eq!(published.category_counts, vec![("portrait".into(), 2)]);
    assert_eq!(
        index.published_members("portrait").expect("members").len(),
        2
    );
    assert_eq!(index.published_uncertain_photo_count().expect("count"), 1);
    assert_eq!(index.published_review_queue().expect("queue").len(), 1);

    let feedback_space = shadow_ai::SemanticEmbeddingSpace::new(
        shadow_ai::SEMANTIC_EMBEDDING_CONTRACT_VERSION,
        "siglip-test@build:space:v1",
        2,
    )
    .expect("feedback space");
    let feedback_embedding = shadow_ai::SemanticEmbedding::new(feedback_space, vec![0.8, 0.6])
        .expect("feedback embedding");
    index
        .store_image("source-b", &feedback_embedding, "test-build")
        .expect("store feedback image embedding");

    index
        .set_user_feedback(
            &second_photo.to_string(),
            &second_representation.to_string(),
            "animals",
            SmartCategoryFeedbackDecision::Belongs,
        )
        .expect("record positive feedback");
    let stored_feedback = index
        .stored_feedback("siglip-test@build:space:v1")
        .expect("load feedback evidence");
    assert_eq!(stored_feedback.len(), 1);
    assert_eq!(stored_feedback[0].category_id, "animals");
    assert_eq!(stored_feedback[0].label, 1);
    assert_eq!(stored_feedback[0].source, "user");
    assert_eq!(stored_feedback[0].embedding.values(), &[0.8, 0.6]);
    assert!(index.adaptation_pending().expect("pending adaptation"));
    assert!(
        smart_classification_snapshot(&root)
            .expect("project pending adaptation")
            .adaptation_pending
    );
    assert_eq!(index.published_uncertain_photo_count().expect("count"), 0);
    assert_eq!(
        index.published_members("animals").expect("members").len(),
        1
    );
    index
        .set_user_feedback(
            &second_photo.to_string(),
            &second_representation.to_string(),
            "animals",
            SmartCategoryFeedbackDecision::DoesNotBelong,
        )
        .expect("record negative feedback");
    assert!(
        index
            .published_members("animals")
            .expect("members")
            .is_empty()
    );

    let replacement = index
        .prepare_run("config-v2", None, true, false)
        .expect("start replacement");
    assert_eq!(replacement.adaptation_revision, 2);
    assert_eq!(
        replacement.embedding_space, "siglip-test@build:space:v1",
        "ordinary refresh reuses the exact published space without provider discovery"
    );
    assert_eq!(replacement.model_build, "test-build");
    assert_eq!(
        index
            .published_snapshot()
            .expect("old published query")
            .expect("old generation remains")
            .config_revision,
        "config-v1"
    );
    index
        .set_user_feedback(
            &second_photo.to_string(),
            &second_representation.to_string(),
            "animals",
            SmartCategoryFeedbackDecision::Belongs,
        )
        .expect("record feedback after run admission");
    index
        .checkpoint_batch(
            &replacement,
            "siglip-test@build:space:v1",
            "test-build",
            2,
            2,
            None,
            &[],
            &[],
            &[],
        )
        .expect("publish replacement");
    let replaced = index
        .published_snapshot()
        .expect("replacement query")
        .expect("replacement generation");
    assert_eq!(replaced.config_revision, "config-v2");
    assert!(replaced.category_counts.is_empty());
    assert!(
        index
            .published_members("portrait")
            .expect("replacement members")
            .is_empty()
    );
    assert!(
        index
            .adaptation_pending()
            .expect("late feedback remains pending"),
        "a run must not clear feedback recorded after it captured its adaptation revision"
    );

    let catchup = index
        .prepare_run("config-v3", None, true, false)
        .expect("start adaptation catch-up");
    assert_eq!(catchup.adaptation_revision, 3);
    index
        .checkpoint_batch(
            &catchup,
            "siglip-test@build:space:v1",
            "test-build",
            2,
            2,
            None,
            &[],
            &[],
            &[],
        )
        .expect("publish adaptation catch-up");
    assert!(!index.adaptation_pending().expect("adaptation applied"));
    assert!(
        !smart_classification_snapshot(&root)
            .expect("project applied adaptation")
            .adaptation_pending
    );

    let rebuild = index
        .prepare_run("config-v4", None, true, true)
        .expect("start explicit rebuild");
    assert!(rebuild.embedding_space.is_empty());
    assert!(rebuild.model_build.is_empty());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn whole_photo_review_records_unchanged_choices_and_clears_uncertainty_atomically() {
    let root = std::env::temp_dir().join(format!(
        "shadow-smart-category-whole-review-{}",
        PhotoId::new_v7()
    ));
    let mut index = SemanticIndex::open(&root).expect("open index");
    let reviewed_photo = PhotoId::new_v7();
    let reviewed_representation = RepresentationId::new_v7();
    let incomplete_photo = PhotoId::new_v7();
    let incomplete_representation = RepresentationId::new_v7();
    let run = index
        .prepare_run("config-v1", None, true, false)
        .expect("start run");
    index
        .checkpoint_batch(
            &run,
            "siglip-test@build:space:v1",
            "test-build",
            2,
            2,
            None,
            &[
                SmartCategoryMatch {
                    photo_id: reviewed_photo,
                    representation_id: reviewed_representation,
                    source_revision: "review-source".into(),
                    category_id: "portrait".into(),
                    cosine_similarity: 0.071,
                },
                SmartCategoryMatch {
                    photo_id: incomplete_photo,
                    representation_id: incomplete_representation,
                    source_revision: "incomplete-source".into(),
                    category_id: "portrait".into(),
                    cosine_similarity: 0.071,
                },
            ],
            &[
                SmartClassifiedPhoto {
                    photo_id: reviewed_photo,
                    representation_id: reviewed_representation,
                    source_revision: "review-source".into(),
                },
                SmartClassifiedPhoto {
                    photo_id: incomplete_photo,
                    representation_id: incomplete_representation,
                    source_revision: "incomplete-source".into(),
                },
            ],
            &[
                SmartCategoryUncertainty {
                    photo_id: reviewed_photo,
                    representation_id: reviewed_representation,
                    source_revision: "review-source".into(),
                    category_id: "portrait".into(),
                    adapted_similarity: 0.071,
                    decision_margin: 0.001,
                },
                SmartCategoryUncertainty {
                    photo_id: reviewed_photo,
                    representation_id: reviewed_representation,
                    source_revision: "review-source".into(),
                    category_id: "travel".into(),
                    adapted_similarity: 0.069,
                    decision_margin: -0.001,
                },
                SmartCategoryUncertainty {
                    photo_id: incomplete_photo,
                    representation_id: incomplete_representation,
                    source_revision: "incomplete-source".into(),
                    category_id: "portrait".into(),
                    adapted_similarity: 0.071,
                    decision_margin: 0.001,
                },
                SmartCategoryUncertainty {
                    photo_id: incomplete_photo,
                    representation_id: incomplete_representation,
                    source_revision: "incomplete-source".into(),
                    category_id: "travel".into(),
                    adapted_similarity: 0.069,
                    decision_margin: -0.001,
                },
            ],
        )
        .expect("publish review candidates");
    let feedback_space = shadow_ai::SemanticEmbeddingSpace::new(
        shadow_ai::SEMANTIC_EMBEDDING_CONTRACT_VERSION,
        "siglip-test@build:space:v1",
        2,
    )
    .expect("feedback space");
    index
        .store_image(
            "review-source",
            &shadow_ai::SemanticEmbedding::new(feedback_space.clone(), vec![0.8, 0.6])
                .expect("review embedding"),
            "test-build",
        )
        .expect("store review embedding");
    index
        .store_image(
            "incomplete-source",
            &shadow_ai::SemanticEmbedding::new(feedback_space, vec![0.6, 0.8])
                .expect("incomplete embedding"),
            "test-build",
        )
        .expect("store incomplete embedding");

    complete_smart_category_review(
        &root,
        reviewed_photo,
        reviewed_representation,
        &[
            SmartCategoryReviewDecision {
                category_id: "portrait".into(),
                decision: SmartCategoryFeedbackDecision::Belongs,
            },
            SmartCategoryReviewDecision {
                category_id: "travel".into(),
                decision: SmartCategoryFeedbackDecision::DoesNotBelong,
            },
        ],
    )
    .expect("complete unchanged whole-photo review");
    assert_eq!(index.published_uncertain_photo_count().expect("count"), 1);
    assert!(
        index
            .published_members("portrait")
            .expect("portrait members")
            .contains(&format!("{reviewed_photo}\u{1f}{reviewed_representation}"))
    );
    assert!(
        index
            .published_members("travel")
            .expect("travel members")
            .is_empty()
    );
    let stored = index
        .stored_feedback("siglip-test@build:space:v1")
        .expect("load complete review feedback");
    assert_eq!(stored.len(), 2);
    assert!(
        stored
            .iter()
            .any(|item| item.category_id == "portrait" && item.label == 1)
    );
    assert!(
        stored
            .iter()
            .any(|item| item.category_id == "travel" && item.label == -1)
    );

    assert!(matches!(
        complete_smart_category_review(
            &root,
            incomplete_photo,
            incomplete_representation,
            &[SmartCategoryReviewDecision {
                category_id: "portrait".into(),
                decision: SmartCategoryFeedbackDecision::DoesNotBelong,
            }],
        ),
        Err(SmartClassificationError::Index(
            SemanticIndexError::IncompleteReview
        ))
    ));
    assert_eq!(index.published_uncertain_photo_count().expect("count"), 1);
    assert_eq!(
        index
            .stored_feedback("siglip-test@build:space:v1")
            .expect("load feedback after rejected review")
            .len(),
        2,
        "an incomplete review must not partially write feedback"
    );
    let replacement = index
        .prepare_run("config-v1", None, true, false)
        .expect("start adaptation run");
    assert_eq!(
        replacement.adaptation_revision, 1,
        "one whole-photo review requests one adaptation revision"
    );
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn schema_one_vector_cache_migrates_without_reembedding() {
    let root = std::env::temp_dir().join(format!(
        "shadow-smart-category-migration-{}",
        PhotoId::new_v7()
    ));
    let database_root = root.join("semantic-index");
    std::fs::create_dir_all(&database_root).expect("create semantic index root");
    let database_path = database_root.join("smart-categories-v1.sqlite3");
    let connection = rusqlite::Connection::open(&database_path).expect("open old index");
    connection
        .execute_batch(
            "CREATE TABLE schema_meta(version INTEGER NOT NULL);
             INSERT INTO schema_meta(version) VALUES (1);
             CREATE TABLE text_embeddings(
                 query_revision TEXT NOT NULL,
                 space_id TEXT NOT NULL,
                 dimensions INTEGER NOT NULL,
                 model_build TEXT NOT NULL,
                 vector BLOB NOT NULL,
                 PRIMARY KEY(query_revision, space_id)
             );
             CREATE TABLE image_embeddings(
                 source_revision TEXT NOT NULL,
                 space_id TEXT NOT NULL,
                 dimensions INTEGER NOT NULL,
                 model_build TEXT NOT NULL,
                 vector BLOB NOT NULL,
                 PRIMARY KEY(source_revision, space_id)
             );",
        )
        .expect("create schema one");
    let values = [1.0f32, 0.0f32]
        .into_iter()
        .flat_map(f32::to_le_bytes)
        .collect::<Vec<_>>();
    connection
        .execute(
            "INSERT INTO text_embeddings(query_revision, space_id, dimensions, model_build, vector)
             VALUES (?1, ?2, 2, ?3, ?4)",
            rusqlite::params![
                "shadow:old-query",
                "siglip-test@build:space:v1",
                "old-build",
                values
            ],
        )
        .expect("insert old vector");
    drop(connection);

    let index = SemanticIndex::open(&root).expect("migrate index");
    let cached = index
        .text("shadow:old-query", "siglip-test@build:space:v1")
        .expect("load migrated vector")
        .expect("preserved vector");
    assert_eq!(cached.embedding.values(), &[1.0, 0.0]);
    assert!(
        index
            .run_checkpoint()
            .expect("new checkpoint table")
            .is_none()
    );
    drop(index);
    let migrated = rusqlite::Connection::open(database_path).expect("reopen migrated index");
    assert_eq!(
        migrated
            .query_row("SELECT version FROM schema_meta", [], |row| row
                .get::<_, i64>(0))
            .expect("schema version"),
        index::INDEX_SCHEMA_VERSION
    );
    assert_eq!(
        migrated
            .query_row(
                "SELECT requested_revision, applied_revision FROM adaptation_state",
                [],
                |row| Ok((row.get::<_, i64>(0)?, row.get::<_, i64>(1)?))
            )
            .expect("adaptation state"),
        (0, 0)
    );
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn schema_three_checkpoint_adds_the_adaptation_revision_in_place() {
    let root = std::env::temp_dir().join(format!(
        "shadow-smart-category-schema-three-{}",
        PhotoId::new_v7()
    ));
    let database_root = root.join("semantic-index");
    std::fs::create_dir_all(&database_root).expect("create semantic index root");
    let database_path = database_root.join("smart-categories-v1.sqlite3");
    let connection = rusqlite::Connection::open(&database_path).expect("open schema three index");
    connection
        .execute_batch(
            "CREATE TABLE schema_meta(version INTEGER NOT NULL);
             INSERT INTO schema_meta(version) VALUES (3);
             CREATE TABLE classification_state(
                 singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),
                 config_revision TEXT NOT NULL,
                 generation TEXT NOT NULL,
                 status TEXT NOT NULL,
                 embedding_space TEXT NOT NULL,
                 model_build TEXT NOT NULL,
                 cursor_path TEXT,
                 cursor_representation_id TEXT,
                 processed_photos INTEGER NOT NULL,
                 total_photos INTEGER NOT NULL,
                 updated_at_ms INTEGER NOT NULL
             );
             INSERT INTO classification_state(
                 singleton_id, config_revision, generation, status, embedding_space,
                 model_build, cursor_path, cursor_representation_id,
                 processed_photos, total_photos, updated_at_ms
             ) VALUES (
                 1, 'config-v1', 'generation-v1', 'complete',
                 'siglip-test@build:space:v1', 'test-build', NULL, NULL, 2, 2, 42
             );",
        )
        .expect("create schema three checkpoint");
    drop(connection);

    let index = SemanticIndex::open(&root).expect("migrate schema three index");
    let checkpoint = index
        .run_checkpoint()
        .expect("load migrated checkpoint")
        .expect("checkpoint remains available");
    assert_eq!(checkpoint.generation, "generation-v1");
    assert_eq!(checkpoint.adaptation_revision, 0);
    assert!(!index.adaptation_pending().expect("initial pending state"));
    drop(index);

    let migrated = rusqlite::Connection::open(database_path).expect("reopen migrated index");
    assert_eq!(
        migrated
            .query_row("SELECT version FROM schema_meta", [], |row| row
                .get::<_, i64>(0))
            .expect("schema version"),
        index::INDEX_SCHEMA_VERSION
    );
    let _ = std::fs::remove_dir_all(root);
}
