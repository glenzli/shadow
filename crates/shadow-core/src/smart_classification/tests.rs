use super::*;
use shadow_catalog::ReviewCursor;
use shadow_domain::{EntityId, PhotoId, RepresentationId};

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
