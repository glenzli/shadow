use rusqlite::params;
use shadow_ai::LearningScope;

use super::{super::*, event_fixtures::catalog_with_exported_event};

#[test]
fn migration_six_creates_immutable_feedback_storage() {
    let catalog = Catalog::open_in_memory().expect("open catalog");
    assert_eq!(
        catalog.schema_version().expect("schema version"),
        crate::schema_v1::SCHEMA_VERSION
    );
    let tables: i64 = catalog
        .connection
        .query_row(
            "SELECT COUNT(*) FROM sqlite_schema
             WHERE type = 'table' AND name IN (
                 'ai_feedback_events', 'ai_feedback_forget_facts'
             )",
            [],
            |row| row.get(0),
        )
        .expect("query feedback tables");
    let triggers: i64 = catalog
        .connection
        .query_row(
            "SELECT COUNT(*) FROM sqlite_schema
             WHERE type = 'trigger' AND name LIKE 'ai_feedback_%_no_%'",
            [],
            |row| row.get(0),
        )
        .expect("query feedback triggers");
    assert_eq!(tables, 2);
    assert_eq!(triggers, 4);
}

#[test]
fn reads_verify_digest_canonical_json_and_indexed_columns() {
    let digest_catalog = catalog_with_exported_event("digest-event");
    digest_catalog
        .connection
        .execute_batch("DROP TRIGGER ai_feedback_events_no_update")
        .expect("disable update guard in corruption fixture");
    digest_catalog
        .connection
        .execute(
            "UPDATE ai_feedback_events SET event_digest = zeroblob(32)
             WHERE event_id = ?1",
            ["digest-event"],
        )
        .expect("corrupt digest fixture");
    assert!(matches!(
        digest_catalog.feedback_events_after(&LearningScope::Global, 0, 10),
        Err(CatalogError::InvalidPersistedFeedback(
            "event JSON digest does not match"
        ))
    ));

    let canonical_catalog = catalog_with_exported_event("canonical-event");
    canonical_catalog
        .connection
        .execute_batch("DROP TRIGGER ai_feedback_events_no_update")
        .expect("disable update guard in canonical fixture");
    let canonical: String = canonical_catalog
        .connection
        .query_row(
            "SELECT event_json FROM ai_feedback_events WHERE event_id = ?1",
            ["canonical-event"],
            |row| row.get(0),
        )
        .expect("read canonical JSON");
    let non_canonical = format!(" {canonical}");
    let non_canonical_digest = blake3::hash(non_canonical.as_bytes());
    canonical_catalog
        .connection
        .execute(
            "UPDATE ai_feedback_events SET event_json = ?1, event_digest = ?2
             WHERE event_id = ?3",
            params![
                non_canonical,
                non_canonical_digest.as_bytes().as_slice(),
                "canonical-event",
            ],
        )
        .expect("corrupt canonical fixture");
    assert!(matches!(
        canonical_catalog.feedback_events_after(&LearningScope::Global, 0, 10),
        Err(CatalogError::InvalidPersistedFeedback(
            "event JSON is not canonical"
        ))
    ));

    let indexed_catalog = catalog_with_exported_event("indexed-event");
    indexed_catalog
        .connection
        .execute_batch("DROP TRIGGER ai_feedback_events_no_update")
        .expect("disable update guard in indexed fixture");
    indexed_catalog
        .connection
        .execute(
            "UPDATE ai_feedback_events SET occurred_at_ms = occurred_at_ms + 1
             WHERE event_id = ?1",
            ["indexed-event"],
        )
        .expect("corrupt indexed fixture");
    assert!(matches!(
        indexed_catalog.feedback_events_after(&LearningScope::Global, 0, 10),
        Err(CatalogError::InvalidPersistedFeedback(
            "event JSON identity disagrees with indexed columns"
        ))
    ));
}
