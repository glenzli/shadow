use std::{
    fs,
    path::Path,
    sync::atomic::{AtomicU64, Ordering},
};

use rusqlite::{Connection, OptionalExtension, Transaction, params};
use shadow_ai::{
    SEMANTIC_EMBEDDING_CONTRACT_VERSION, SemanticContractError, SemanticEmbedding,
    SemanticEmbeddingSpace,
};
use shadow_catalog::ReviewCursor;
use thiserror::Error;
use time::OffsetDateTime;

use super::{
    MAXIMUM_REVIEW_QUEUE_ITEMS, SmartCategoryFeedbackDecision, SmartCategoryMatch,
    SmartCategoryReviewItem, SmartCategoryUncertainty, SmartClassifiedPhoto,
};

pub(super) const INDEX_SCHEMA_VERSION: i64 = 2026081101;

#[derive(Debug, Clone, PartialEq)]
pub(super) struct StoredFeedback {
    pub category_id: String,
    pub photo_id: String,
    pub representation_id: String,
    pub label: i8,
    pub source: String,
    pub embedding: SemanticEmbedding,
}

pub(super) struct CachedEmbedding {
    pub embedding: SemanticEmbedding,
    pub model_build: String,
}

#[derive(Debug, Clone, PartialEq)]
pub(super) struct RunCheckpoint {
    pub config_revision: String,
    pub generation: String,
    pub status: String,
    pub embedding_space: String,
    pub model_build: String,
    pub cursor: Option<ReviewCursor>,
    pub processed_photos: u64,
    pub total_photos: u64,
    pub adaptation_revision: u64,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub(super) struct PublishedSnapshot {
    pub config_revision: String,
    pub generation: String,
    pub embedding_space: String,
    pub model_build: String,
    pub total_photos: u64,
    pub published_at_ms: i64,
    pub category_counts: Vec<(String, u64)>,
}

pub(super) struct SemanticIndex {
    connection: Connection,
}

impl SemanticIndex {
    pub fn open(cache_root: &Path) -> Result<Self, SemanticIndexError> {
        let root = cache_root.join("semantic-index");
        fs::create_dir_all(&root)?;
        let connection = Connection::open(root.join("smart-categories-v1.sqlite3"))?;
        connection.execute_batch(
            "PRAGMA journal_mode=WAL;
             PRAGMA synchronous=NORMAL;
             CREATE TABLE IF NOT EXISTS schema_meta(version INTEGER NOT NULL);
             CREATE TABLE IF NOT EXISTS text_embeddings(
                 query_revision TEXT NOT NULL,
                 space_id TEXT NOT NULL,
                 dimensions INTEGER NOT NULL,
                 model_build TEXT NOT NULL,
                 vector BLOB NOT NULL,
                 PRIMARY KEY(query_revision, space_id)
             );
             CREATE TABLE IF NOT EXISTS image_embeddings(
                 source_revision TEXT NOT NULL,
                 space_id TEXT NOT NULL,
                 dimensions INTEGER NOT NULL,
                 model_build TEXT NOT NULL,
                 vector BLOB NOT NULL,
                 PRIMARY KEY(source_revision, space_id)
             );
             CREATE TABLE IF NOT EXISTS classification_state(
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
                 adaptation_revision INTEGER NOT NULL DEFAULT 0,
                 updated_at_ms INTEGER NOT NULL
             );
             CREATE TABLE IF NOT EXISTS published_classification(
                 singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),
                 config_revision TEXT NOT NULL,
                 generation TEXT NOT NULL,
                 embedding_space TEXT NOT NULL,
                 model_build TEXT NOT NULL,
                 total_photos INTEGER NOT NULL,
                 published_at_ms INTEGER NOT NULL
             );
             CREATE TABLE IF NOT EXISTS category_memberships(
                 generation TEXT NOT NULL,
                 config_revision TEXT NOT NULL,
                 category_id TEXT NOT NULL,
                 photo_id TEXT NOT NULL,
                 representation_id TEXT NOT NULL,
                 source_revision TEXT NOT NULL,
                 cosine_similarity REAL NOT NULL,
                 PRIMARY KEY(generation, category_id, photo_id, representation_id)
             );
             CREATE INDEX IF NOT EXISTS category_memberships_lookup
                 ON category_memberships(generation, category_id);
             CREATE TABLE IF NOT EXISTS classified_photos(
                 generation TEXT NOT NULL,
                 config_revision TEXT NOT NULL,
                 photo_id TEXT NOT NULL,
                 representation_id TEXT NOT NULL,
                 source_revision TEXT NOT NULL,
                 PRIMARY KEY(generation, photo_id, representation_id)
             );
             CREATE TABLE IF NOT EXISTS category_uncertainties(
                 generation TEXT NOT NULL,
                 config_revision TEXT NOT NULL,
                 category_id TEXT NOT NULL,
                 photo_id TEXT NOT NULL,
                 representation_id TEXT NOT NULL,
                 source_revision TEXT NOT NULL,
                 adapted_similarity REAL NOT NULL,
                 decision_margin REAL NOT NULL,
                 PRIMARY KEY(generation, category_id, photo_id, representation_id)
             );
             CREATE INDEX IF NOT EXISTS category_uncertainties_lookup
                 ON category_uncertainties(generation, decision_margin, photo_id, representation_id);
             CREATE TABLE IF NOT EXISTS category_feedback(
                 category_id TEXT NOT NULL,
                 photo_id TEXT NOT NULL,
                 representation_id TEXT NOT NULL,
                 source_revision TEXT NOT NULL,
                 source TEXT NOT NULL CHECK(source IN ('user', 'advanced_model')),
                 label INTEGER NOT NULL CHECK(label IN (-1, 1)),
                 updated_at_ms INTEGER NOT NULL,
                 PRIMARY KEY(category_id, photo_id, representation_id, source)
             );
             CREATE TABLE IF NOT EXISTS adaptation_state(
                 singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),
                 requested_revision INTEGER NOT NULL,
                 applied_revision INTEGER NOT NULL,
                 updated_at_ms INTEGER NOT NULL
             );",
        )?;
        connection.execute(
            "INSERT OR IGNORE INTO adaptation_state(
                 singleton_id, requested_revision, applied_revision, updated_at_ms
             ) VALUES (1, 0, 0, ?1)",
            [now_ms()],
        )?;
        let version: Option<i64> = connection
            .query_row("SELECT version FROM schema_meta LIMIT 1", [], |row| {
                row.get(0)
            })
            .optional()?;
        match version {
            None => {
                connection.execute(
                    "INSERT INTO schema_meta(version) VALUES (?1)",
                    [INDEX_SCHEMA_VERSION],
                )?;
            }
            Some(1 | 2 | 3) => {
                if !table_has_column(&connection, "classification_state", "adaptation_revision")? {
                    connection.execute(
                        "ALTER TABLE classification_state
                         ADD COLUMN adaptation_revision INTEGER NOT NULL DEFAULT 0",
                        [],
                    )?;
                }
                connection.execute(
                    "UPDATE schema_meta SET version = ?1",
                    [INDEX_SCHEMA_VERSION],
                )?;
            }
            Some(INDEX_SCHEMA_VERSION) => {}
            Some(_) => return Err(SemanticIndexError::UnsupportedSchema),
        }
        Ok(Self { connection })
    }

    pub fn text(
        &self,
        revision: &str,
        space: &str,
    ) -> Result<Option<CachedEmbedding>, SemanticIndexError> {
        self.load("text_embeddings", "query_revision", revision, space)
    }

    pub fn image(
        &self,
        revision: &str,
        space: &str,
    ) -> Result<Option<CachedEmbedding>, SemanticIndexError> {
        self.load("image_embeddings", "source_revision", revision, space)
    }

    pub fn store_text(
        &self,
        revision: &str,
        embedding: &SemanticEmbedding,
        model_build: &str,
    ) -> Result<(), SemanticIndexError> {
        self.store(
            "text_embeddings",
            "query_revision",
            revision,
            embedding,
            model_build,
        )
    }

    pub fn store_image(
        &self,
        revision: &str,
        embedding: &SemanticEmbedding,
        model_build: &str,
    ) -> Result<(), SemanticIndexError> {
        self.store(
            "image_embeddings",
            "source_revision",
            revision,
            embedding,
            model_build,
        )
    }

    pub fn prepare_run(
        &mut self,
        config_revision: &str,
        requested_generation: Option<&str>,
        start_new: bool,
        clear_embeddings: bool,
    ) -> Result<RunCheckpoint, SemanticIndexError> {
        if !start_new {
            let checkpoint = self
                .run_checkpoint()?
                .ok_or(SemanticIndexError::InvalidCheckpoint)?;
            if checkpoint.config_revision != config_revision
                || requested_generation.is_none_or(|generation| generation != checkpoint.generation)
                || checkpoint.status == "complete"
            {
                return Err(SemanticIndexError::InvalidCheckpoint);
            }
            let changed = self.connection.execute(
                "UPDATE classification_state SET status = 'running', updated_at_ms = ?1 WHERE singleton_id = 1",
                [now_ms()],
            )?;
            if changed != 1 {
                return Err(SemanticIndexError::InvalidCheckpoint);
            }
            return Ok(RunCheckpoint {
                status: "running".into(),
                ..checkpoint
            });
        }
        self.start_run(config_revision, clear_embeddings)
    }

    fn start_run(
        &mut self,
        config_revision: &str,
        clear_embeddings: bool,
    ) -> Result<RunCheckpoint, SemanticIndexError> {
        let generation = generation_id(config_revision);
        let now = now_ms();
        let transaction = self.connection.transaction()?;
        let adaptation_revision = transaction.query_row(
            "SELECT requested_revision FROM adaptation_state WHERE singleton_id = 1",
            [],
            |row| row_u64(row, 0),
        )?;
        if clear_embeddings {
            transaction.execute("DELETE FROM image_embeddings", [])?;
            transaction.execute("DELETE FROM text_embeddings", [])?;
        }
        let published: Option<(String, String, String)> = transaction
            .query_row(
                "SELECT generation, embedding_space, model_build
                 FROM published_classification WHERE singleton_id = 1",
                [],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .optional()?;
        if let Some((published_generation, _, _)) = published.as_ref() {
            transaction.execute(
                "DELETE FROM category_memberships WHERE generation <> ?1",
                [published_generation],
            )?;
            transaction.execute(
                "DELETE FROM classified_photos WHERE generation <> ?1",
                [published_generation],
            )?;
            transaction.execute(
                "DELETE FROM category_uncertainties WHERE generation <> ?1",
                [published_generation],
            )?;
        } else {
            transaction.execute("DELETE FROM category_memberships", [])?;
            transaction.execute("DELETE FROM classified_photos", [])?;
            transaction.execute("DELETE FROM category_uncertainties", [])?;
        }
        let (embedding_space, model_build) = if clear_embeddings {
            ("", "")
        } else {
            published.as_ref().map_or(("", ""), |(_, space, build)| {
                (space.as_str(), build.as_str())
            })
        };
        transaction.execute(
            "INSERT OR REPLACE INTO classification_state(
                 singleton_id, config_revision, generation, status, embedding_space, model_build,
                 cursor_path, cursor_representation_id, processed_photos, total_photos,
                 adaptation_revision, updated_at_ms
             ) VALUES (1, ?1, ?2, 'running', ?3, ?4, NULL, NULL, 0, 0, ?5, ?6)",
            params![
                config_revision,
                generation,
                embedding_space,
                model_build,
                sql_u64(adaptation_revision)?,
                now
            ],
        )?;
        transaction.commit()?;
        Ok(RunCheckpoint {
            config_revision: config_revision.into(),
            generation,
            status: "running".into(),
            embedding_space: embedding_space.into(),
            model_build: model_build.into(),
            cursor: None,
            processed_photos: 0,
            total_photos: 0,
            adaptation_revision,
            updated_at_ms: now,
        })
    }

    pub fn run_checkpoint(&self) -> Result<Option<RunCheckpoint>, SemanticIndexError> {
        self.connection
            .query_row(
                "SELECT config_revision, generation, status, embedding_space, model_build,
                        cursor_path, cursor_representation_id, processed_photos, total_photos,
                        adaptation_revision, updated_at_ms
                 FROM classification_state WHERE singleton_id = 1",
                [],
                |row| {
                    let cursor_path: Option<String> = row.get(5)?;
                    let cursor_representation_id: Option<String> = row.get(6)?;
                    let cursor = match (cursor_path, cursor_representation_id) {
                        (Some(display_path), Some(representation_id)) => Some(ReviewCursor {
                            display_path,
                            representation_id: representation_id.parse().map_err(|error| {
                                rusqlite::Error::FromSqlConversionFailure(
                                    6,
                                    rusqlite::types::Type::Text,
                                    Box::new(error),
                                )
                            })?,
                        }),
                        (None, None) => None,
                        _ => return Err(rusqlite::Error::InvalidQuery),
                    };
                    Ok(RunCheckpoint {
                        config_revision: row.get(0)?,
                        generation: row.get(1)?,
                        status: row.get(2)?,
                        embedding_space: row.get(3)?,
                        model_build: row.get(4)?,
                        cursor,
                        processed_photos: row_u64(row, 7)?,
                        total_photos: row_u64(row, 8)?,
                        adaptation_revision: row_u64(row, 9)?,
                        updated_at_ms: row.get(10)?,
                    })
                },
            )
            .optional()
            .map_err(Into::into)
    }

    pub fn checkpoint_batch(
        &mut self,
        checkpoint: &RunCheckpoint,
        embedding_space: &str,
        model_build: &str,
        processed_in_batch: usize,
        total_photos: u64,
        next_cursor: Option<&ReviewCursor>,
        matches: &[SmartCategoryMatch],
        photos: &[SmartClassifiedPhoto],
        uncertainties: &[SmartCategoryUncertainty],
    ) -> Result<RunCheckpoint, SemanticIndexError> {
        let next_processed = checkpoint.processed_photos.saturating_add(
            u64::try_from(processed_in_batch).map_err(|_| SemanticIndexError::InvalidCount)?,
        );
        let now = now_ms();
        let next_processed_sql = sql_u64(next_processed)?;
        let total_photos_sql = sql_u64(total_photos)?;
        let transaction = self.connection.transaction()?;
        insert_memberships(&transaction, checkpoint, matches)?;
        insert_classified_photos(&transaction, checkpoint, photos)?;
        insert_uncertainties(&transaction, checkpoint, uncertainties)?;
        let (cursor_path, cursor_representation_id) = next_cursor
            .map(|cursor| {
                (
                    Some(cursor.display_path.as_str()),
                    Some(cursor.representation_id.to_string()),
                )
            })
            .unwrap_or((None, None));
        let status = if next_cursor.is_some() {
            "running"
        } else {
            "complete"
        };
        let changed = transaction.execute(
            "UPDATE classification_state
             SET status = ?1, embedding_space = ?2, model_build = ?3,
                 cursor_path = ?4, cursor_representation_id = ?5,
                 processed_photos = ?6, total_photos = ?7, updated_at_ms = ?8
             WHERE singleton_id = 1 AND generation = ?9",
            params![
                status,
                embedding_space,
                model_build,
                cursor_path,
                cursor_representation_id,
                next_processed_sql,
                total_photos_sql,
                now,
                checkpoint.generation,
            ],
        )?;
        if changed != 1 {
            return Err(SemanticIndexError::InvalidCheckpoint);
        }
        if next_cursor.is_none() {
            publish_generation(
                &transaction,
                checkpoint,
                embedding_space,
                model_build,
                total_photos,
                now,
            )?;
        }
        transaction.commit()?;
        Ok(RunCheckpoint {
            config_revision: checkpoint.config_revision.clone(),
            generation: checkpoint.generation.clone(),
            status: status.into(),
            embedding_space: embedding_space.into(),
            model_build: model_build.into(),
            cursor: next_cursor.cloned(),
            processed_photos: next_processed,
            total_photos,
            adaptation_revision: checkpoint.adaptation_revision,
            updated_at_ms: now,
        })
    }

    pub fn mark_paused(&self, generation: &str) -> Result<(), SemanticIndexError> {
        self.set_terminal_status(generation, "paused")
    }

    pub fn mark_failed(&self, generation: &str) -> Result<(), SemanticIndexError> {
        self.set_terminal_status(generation, "failed")
    }

    fn set_terminal_status(
        &self,
        generation: &str,
        status: &str,
    ) -> Result<(), SemanticIndexError> {
        let changed = self.connection.execute(
            "UPDATE classification_state SET status = ?1, updated_at_ms = ?2
             WHERE singleton_id = 1 AND generation = ?3 AND status <> 'complete'",
            params![status, now_ms(), generation],
        )?;
        if changed != 1 {
            return Err(SemanticIndexError::InvalidCheckpoint);
        }
        Ok(())
    }

    pub fn published_snapshot(&self) -> Result<Option<PublishedSnapshot>, SemanticIndexError> {
        let header: Option<(String, String, String, String, u64, i64)> = self
            .connection
            .query_row(
                "SELECT config_revision, generation, embedding_space, model_build,
                        total_photos, published_at_ms
                 FROM published_classification WHERE singleton_id = 1",
                [],
                |row| {
                    Ok((
                        row.get(0)?,
                        row.get(1)?,
                        row.get(2)?,
                        row.get(3)?,
                        row_u64(row, 4)?,
                        row.get(5)?,
                    ))
                },
            )
            .optional()?;
        let Some((
            config_revision,
            generation,
            embedding_space,
            model_build,
            total_photos,
            published_at_ms,
        )) = header
        else {
            return Ok(None);
        };
        let mut statement = self.connection.prepare(
            "SELECT category_id, COUNT(*) FROM category_memberships
             WHERE generation = ?1 GROUP BY category_id ORDER BY category_id",
        )?;
        let category_counts = statement
            .query_map([&generation], |row| Ok((row.get(0)?, row_u64(row, 1)?)))?
            .collect::<Result<Vec<(String, u64)>, _>>()?;
        Ok(Some(PublishedSnapshot {
            config_revision,
            generation,
            embedding_space,
            model_build,
            total_photos,
            published_at_ms,
            category_counts,
        }))
    }

    pub fn adaptation_pending(&self) -> Result<bool, SemanticIndexError> {
        self.connection
            .query_row(
                "SELECT requested_revision > applied_revision
                 FROM adaptation_state WHERE singleton_id = 1",
                [],
                |row| row.get(0),
            )
            .map_err(Into::into)
    }

    pub fn published_members(&self, category_id: &str) -> Result<Vec<String>, SemanticIndexError> {
        let generation: Option<String> = self
            .connection
            .query_row(
                "SELECT generation FROM published_classification WHERE singleton_id = 1",
                [],
                |row| row.get(0),
            )
            .optional()?;
        let Some(generation) = generation else {
            return Ok(Vec::new());
        };
        let mut statement = self.connection.prepare(
            "SELECT photo_id, representation_id FROM category_memberships
             WHERE generation = ?1 AND category_id = ?2
             ORDER BY photo_id, representation_id",
        )?;
        statement
            .query_map(params![generation, category_id], |row| {
                let photo_id: String = row.get(0)?;
                let representation_id: String = row.get(1)?;
                Ok(format!("{photo_id}\u{1f}{representation_id}"))
            })?
            .collect::<Result<Vec<_>, _>>()
            .map_err(Into::into)
    }

    pub fn published_uncertain_photo_count(&self) -> Result<u64, SemanticIndexError> {
        let generation = self.published_generation()?;
        let Some(generation) = generation else {
            return Ok(0);
        };
        self.connection
            .query_row(
                "SELECT COUNT(*) FROM (
                     SELECT 1 FROM category_uncertainties
                     WHERE generation = ?1
                     GROUP BY photo_id, representation_id
                 )",
                [&generation],
                |row| row_u64(row, 0),
            )
            .map_err(Into::into)
    }

    pub fn published_review_queue(
        &self,
    ) -> Result<Vec<SmartCategoryReviewItem>, SemanticIndexError> {
        let generation = self.published_generation()?;
        let Some(generation) = generation else {
            return Ok(Vec::new());
        };
        let mut statement = self.connection.prepare(
            "SELECT photo_id, representation_id, category_id,
                    adapted_similarity, decision_margin
             FROM category_uncertainties
             WHERE generation = ?1
             ORDER BY ABS(decision_margin), photo_id, representation_id, category_id
             LIMIT ?2",
        )?;
        statement
            .query_map(
                params![
                    generation,
                    i64::try_from(MAXIMUM_REVIEW_QUEUE_ITEMS)
                        .map_err(|_| SemanticIndexError::InvalidCount)?
                ],
                |row| {
                    let photo_id: String = row.get(0)?;
                    let representation_id: String = row.get(1)?;
                    Ok(SmartCategoryReviewItem {
                        photo_id: photo_id.parse().map_err(|error| {
                            rusqlite::Error::FromSqlConversionFailure(
                                0,
                                rusqlite::types::Type::Text,
                                Box::new(error),
                            )
                        })?,
                        representation_id: representation_id.parse().map_err(|error| {
                            rusqlite::Error::FromSqlConversionFailure(
                                1,
                                rusqlite::types::Type::Text,
                                Box::new(error),
                            )
                        })?,
                        category_id: row.get(2)?,
                        adapted_similarity: row.get(3)?,
                        decision_margin: row.get(4)?,
                    })
                },
            )?
            .collect::<Result<Vec<_>, _>>()
            .map_err(Into::into)
    }

    pub fn stored_feedback(&self, space: &str) -> Result<Vec<StoredFeedback>, SemanticIndexError> {
        let mut statement = self.connection.prepare(
            "SELECT feedback.category_id, feedback.photo_id,
                    feedback.representation_id, feedback.label, feedback.source,
                    vectors.dimensions, vectors.vector
             FROM category_feedback AS feedback
             JOIN image_embeddings AS vectors
               ON vectors.source_revision = feedback.source_revision
              AND vectors.space_id = ?1
             ORDER BY feedback.updated_at_ms DESC",
        )?;
        statement
            .query_map([space], |row| {
                let dimensions: i64 = row.get(5)?;
                let dimensions = usize::try_from(dimensions).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        5,
                        rusqlite::types::Type::Integer,
                        Box::new(error),
                    )
                })?;
                let bytes: Vec<u8> = row.get(6)?;
                let values = decode_values(&bytes, dimensions).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        6,
                        rusqlite::types::Type::Blob,
                        Box::new(error),
                    )
                })?;
                let semantic_space = SemanticEmbeddingSpace::new(
                    SEMANTIC_EMBEDDING_CONTRACT_VERSION,
                    space,
                    dimensions,
                )
                .map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        6,
                        rusqlite::types::Type::Blob,
                        Box::new(error),
                    )
                })?;
                let embedding =
                    SemanticEmbedding::new(semantic_space, values).map_err(|error| {
                        rusqlite::Error::FromSqlConversionFailure(
                            6,
                            rusqlite::types::Type::Blob,
                            Box::new(error),
                        )
                    })?;
                Ok(StoredFeedback {
                    category_id: row.get(0)?,
                    photo_id: row.get(1)?,
                    representation_id: row.get(2)?,
                    label: row.get(3)?,
                    source: row.get(4)?,
                    embedding,
                })
            })?
            .collect::<Result<Vec<_>, _>>()
            .map_err(Into::into)
    }

    pub fn set_user_feedback(
        &mut self,
        photo_id: &str,
        representation_id: &str,
        category_id: &str,
        decision: SmartCategoryFeedbackDecision,
    ) -> Result<(), SemanticIndexError> {
        let generation = self
            .published_generation()?
            .ok_or(SemanticIndexError::UnknownPublishedPhoto)?;
        let source_revision: String = self
            .connection
            .query_row(
                "SELECT source_revision FROM classified_photos
                 WHERE generation = ?1 AND photo_id = ?2 AND representation_id = ?3",
                params![generation, photo_id, representation_id],
                |row| row.get(0),
            )
            .optional()?
            .ok_or(SemanticIndexError::UnknownPublishedPhoto)?;
        let transaction = self.connection.transaction()?;
        transaction.execute(
            "DELETE FROM category_feedback
             WHERE category_id = ?1 AND photo_id = ?2 AND representation_id = ?3
               AND source = 'user'",
            params![category_id, photo_id, representation_id],
        )?;
        transaction.execute(
            "DELETE FROM category_uncertainties
             WHERE generation = ?1 AND category_id = ?2
               AND photo_id = ?3 AND representation_id = ?4",
            params![generation, category_id, photo_id, representation_id],
        )?;
        match decision {
            SmartCategoryFeedbackDecision::Clear => {
                transaction.execute(
                    "DELETE FROM category_memberships
                     WHERE generation = ?1 AND category_id = ?2
                       AND photo_id = ?3 AND representation_id = ?4",
                    params![generation, category_id, photo_id, representation_id],
                )?;
            }
            SmartCategoryFeedbackDecision::DoesNotBelong
            | SmartCategoryFeedbackDecision::Belongs => {
                let label = if decision == SmartCategoryFeedbackDecision::Belongs {
                    1
                } else {
                    -1
                };
                transaction.execute(
                    "INSERT INTO category_feedback(
                         category_id, photo_id, representation_id, source_revision,
                         source, label, updated_at_ms
                     ) VALUES (?1, ?2, ?3, ?4, 'user', ?5, ?6)",
                    params![
                        category_id,
                        photo_id,
                        representation_id,
                        source_revision,
                        label,
                        now_ms()
                    ],
                )?;
                if decision == SmartCategoryFeedbackDecision::Belongs {
                    let config_revision: String = transaction.query_row(
                        "SELECT config_revision FROM published_classification WHERE singleton_id = 1",
                        [],
                        |row| row.get(0),
                    )?;
                    transaction.execute(
                        "INSERT OR REPLACE INTO category_memberships(
                             generation, config_revision, category_id, photo_id,
                             representation_id, source_revision, cosine_similarity
                         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 1.0)",
                        params![
                            generation,
                            config_revision,
                            category_id,
                            photo_id,
                            representation_id,
                            source_revision
                        ],
                    )?;
                } else {
                    transaction.execute(
                        "DELETE FROM category_memberships
                         WHERE generation = ?1 AND category_id = ?2
                           AND photo_id = ?3 AND representation_id = ?4",
                        params![generation, category_id, photo_id, representation_id],
                    )?;
                }
            }
        }
        let changed = transaction.execute(
            "UPDATE adaptation_state
             SET requested_revision = requested_revision + 1, updated_at_ms = ?1
             WHERE singleton_id = 1",
            [now_ms()],
        )?;
        if changed != 1 {
            return Err(SemanticIndexError::InvalidCheckpoint);
        }
        transaction.commit()?;
        Ok(())
    }

    fn published_generation(&self) -> Result<Option<String>, SemanticIndexError> {
        self.connection
            .query_row(
                "SELECT generation FROM published_classification WHERE singleton_id = 1",
                [],
                |row| row.get(0),
            )
            .optional()
            .map_err(Into::into)
    }

    fn load(
        &self,
        table: &str,
        revision_column: &str,
        revision: &str,
        space: &str,
    ) -> Result<Option<CachedEmbedding>, SemanticIndexError> {
        let sql = format!(
            "SELECT dimensions, model_build, vector FROM {table} WHERE {revision_column} = ?1 AND space_id = ?2"
        );
        let row: Option<(i64, String, Vec<u8>)> = self
            .connection
            .query_row(&sql, params![revision, space], |row| {
                Ok((row.get(0)?, row.get(1)?, row.get(2)?))
            })
            .optional()?;
        row.map(|(dimensions, model_build, bytes)| {
            let dimensions =
                usize::try_from(dimensions).map_err(|_| SemanticIndexError::InvalidVector)?;
            let values = decode_values(&bytes, dimensions)?;
            let semantic_space = SemanticEmbeddingSpace::new(
                SEMANTIC_EMBEDDING_CONTRACT_VERSION,
                space,
                dimensions,
            )?;
            Ok(CachedEmbedding {
                embedding: SemanticEmbedding::new(semantic_space, values)?,
                model_build,
            })
        })
        .transpose()
    }

    fn store(
        &self,
        table: &str,
        revision_column: &str,
        revision: &str,
        embedding: &SemanticEmbedding,
        model_build: &str,
    ) -> Result<(), SemanticIndexError> {
        let sql = format!(
            "INSERT OR REPLACE INTO {table}({revision_column}, space_id, dimensions, model_build, vector) VALUES (?1, ?2, ?3, ?4, ?5)"
        );
        self.connection.execute(
            &sql,
            params![
                revision,
                embedding.space().space_id(),
                i64::try_from(embedding.space().dimensions())
                    .map_err(|_| SemanticIndexError::InvalidVector)?,
                model_build,
                encode_values(embedding.values())
            ],
        )?;
        Ok(())
    }
}

fn insert_memberships(
    transaction: &Transaction<'_>,
    checkpoint: &RunCheckpoint,
    matches: &[SmartCategoryMatch],
) -> Result<(), SemanticIndexError> {
    let mut statement = transaction.prepare_cached(
        "INSERT OR REPLACE INTO category_memberships(
             generation, config_revision, category_id, photo_id, representation_id,
             source_revision, cosine_similarity
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
    )?;
    for matched in matches {
        statement.execute(params![
            checkpoint.generation,
            checkpoint.config_revision,
            matched.category_id,
            matched.photo_id.to_string(),
            matched.representation_id.to_string(),
            matched.source_revision,
            matched.cosine_similarity,
        ])?;
    }
    Ok(())
}

fn insert_classified_photos(
    transaction: &Transaction<'_>,
    checkpoint: &RunCheckpoint,
    photos: &[SmartClassifiedPhoto],
) -> Result<(), SemanticIndexError> {
    let mut statement = transaction.prepare_cached(
        "INSERT OR REPLACE INTO classified_photos(
             generation, config_revision, photo_id, representation_id, source_revision
         ) VALUES (?1, ?2, ?3, ?4, ?5)",
    )?;
    for photo in photos {
        statement.execute(params![
            checkpoint.generation,
            checkpoint.config_revision,
            photo.photo_id.to_string(),
            photo.representation_id.to_string(),
            photo.source_revision,
        ])?;
        transaction.execute(
            "UPDATE category_feedback SET source_revision = ?1
             WHERE photo_id = ?2 AND representation_id = ?3",
            params![
                photo.source_revision,
                photo.photo_id.to_string(),
                photo.representation_id.to_string()
            ],
        )?;
    }
    Ok(())
}

fn insert_uncertainties(
    transaction: &Transaction<'_>,
    checkpoint: &RunCheckpoint,
    uncertainties: &[SmartCategoryUncertainty],
) -> Result<(), SemanticIndexError> {
    let mut statement = transaction.prepare_cached(
        "INSERT OR REPLACE INTO category_uncertainties(
             generation, config_revision, category_id, photo_id, representation_id,
             source_revision, adapted_similarity, decision_margin
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
    )?;
    for uncertain in uncertainties {
        statement.execute(params![
            checkpoint.generation,
            checkpoint.config_revision,
            uncertain.category_id,
            uncertain.photo_id.to_string(),
            uncertain.representation_id.to_string(),
            uncertain.source_revision,
            uncertain.adapted_similarity,
            uncertain.decision_margin,
        ])?;
    }
    Ok(())
}

fn publish_generation(
    transaction: &Transaction<'_>,
    checkpoint: &RunCheckpoint,
    embedding_space: &str,
    model_build: &str,
    total_photos: u64,
    now: i64,
) -> Result<(), SemanticIndexError> {
    let total_photos_sql = sql_u64(total_photos)?;
    transaction.execute(
        "INSERT OR REPLACE INTO published_classification(
             singleton_id, config_revision, generation, embedding_space, model_build,
             total_photos, published_at_ms
         ) VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            checkpoint.config_revision,
            checkpoint.generation,
            embedding_space,
            model_build,
            total_photos_sql,
            now,
        ],
    )?;
    transaction.execute(
        "DELETE FROM category_memberships WHERE generation <> ?1",
        [&checkpoint.generation],
    )?;
    transaction.execute(
        "DELETE FROM classified_photos WHERE generation <> ?1",
        [&checkpoint.generation],
    )?;
    transaction.execute(
        "DELETE FROM category_uncertainties WHERE generation <> ?1",
        [&checkpoint.generation],
    )?;
    transaction.execute(
        "UPDATE adaptation_state
         SET applied_revision = MAX(applied_revision, ?1), updated_at_ms = ?2
         WHERE singleton_id = 1",
        params![sql_u64(checkpoint.adaptation_revision)?, now],
    )?;
    Ok(())
}

fn table_has_column(
    connection: &Connection,
    table: &str,
    column: &str,
) -> Result<bool, SemanticIndexError> {
    let mut statement = connection.prepare(&format!("PRAGMA table_info({table})"))?;
    let mut rows = statement.query([])?;
    while let Some(row) = rows.next()? {
        let existing: String = row.get(1)?;
        if existing == column {
            return Ok(true);
        }
    }
    Ok(false)
}

fn generation_id(config_revision: &str) -> String {
    static GENERATION_SEQUENCE: AtomicU64 = AtomicU64::new(0);
    let timestamp = OffsetDateTime::now_utc().unix_timestamp_nanos();
    let sequence = GENERATION_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let digest = blake3::hash(format!("{config_revision}\0{timestamp}\0{sequence}").as_bytes());
    format!("smart-{timestamp}-{sequence}-{}", &digest.to_hex()[..16])
}

fn now_ms() -> i64 {
    let milliseconds = OffsetDateTime::now_utc().unix_timestamp_nanos() / 1_000_000;
    i64::try_from(milliseconds).unwrap_or_else(|_| {
        if milliseconds.is_negative() {
            i64::MIN
        } else {
            i64::MAX
        }
    })
}

fn sql_u64(value: u64) -> Result<i64, SemanticIndexError> {
    i64::try_from(value).map_err(|_| SemanticIndexError::InvalidCount)
}

fn row_u64(row: &rusqlite::Row<'_>, index: usize) -> rusqlite::Result<u64> {
    let value: i64 = row.get(index)?;
    u64::try_from(value).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(
            index,
            rusqlite::types::Type::Integer,
            Box::new(error),
        )
    })
}

fn encode_values(values: &[f32]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(values.len() * 4);
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    bytes
}

fn decode_values(bytes: &[u8], dimensions: usize) -> Result<Vec<f32>, SemanticIndexError> {
    if bytes.len() != dimensions.saturating_mul(4) {
        return Err(SemanticIndexError::InvalidVector);
    }
    Ok(bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect())
}

#[derive(Debug, Error)]
pub enum SemanticIndexError {
    #[error("smart category index schema is unsupported")]
    UnsupportedSchema,
    #[error("smart category checkpoint is incompatible with this request")]
    InvalidCheckpoint,
    #[error("smart category index contains an invalid vector")]
    InvalidVector,
    #[error("smart category index contains an invalid count")]
    InvalidCount,
    #[error("smart category feedback references a photo outside the published classification")]
    UnknownPublishedPhoto,
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error(transparent)]
    Sqlite(#[from] rusqlite::Error),
    #[error(transparent)]
    Contract(#[from] SemanticContractError),
}
