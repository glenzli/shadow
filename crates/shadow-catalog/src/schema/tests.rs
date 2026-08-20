use super::*;
use crate::{Catalog, catalog::configure_connection};

#[test]
fn creates_current_catalog_shape() {
    let catalog = Catalog::open_in_memory().expect("open catalog");

    assert_eq!(
        catalog.schema_version().expect("schema revision"),
        SCHEMA_VERSION
    );
    let raw_frame_column: i64 = catalog
        .connection
        .query_row(
            "SELECT COUNT(*) FROM pragma_table_info('representation_decode_snapshots')
             WHERE name = 'can_decode_raw_frame'",
            [],
            |row| row.get(0),
        )
        .expect("read v1 RawFrame capability column");
    assert_eq!(raw_frame_column, 1);
    let identity_source_columns: i64 = catalog
        .connection
        .query_row(
            "SELECT COUNT(*) FROM pragma_table_info('representation_content_identities')
             WHERE name IN ('source_byte_len', 'source_modified_at_ms')",
            [],
            |row| row.get(0),
        )
        .expect("read identity source provenance columns");
    assert_eq!(identity_source_columns, 2);
    let export_queue_table: i64 = catalog
        .connection
        .query_row(
            "SELECT EXISTS(
                 SELECT 1 FROM sqlite_schema WHERE type = 'table' AND name = 'export_jobs'
             )",
            [],
            |row| row.get(0),
        )
        .expect("read v1 durable export queue table");
    assert_eq!(export_queue_table, 1);
    let logical_photo_group_table: i64 = catalog
        .connection
        .query_row(
            "SELECT EXISTS(
                 SELECT 1 FROM sqlite_schema
                 WHERE type = 'table' AND name = 'import_photo_groups'
             )",
            [],
            |row| row.get(0),
        )
        .expect("read logical-photo companion group table");
    assert_eq!(logical_photo_group_table, 1);
    let photo_relationship_tables: i64 = catalog
        .connection
        .query_row(
            "SELECT COUNT(*) FROM sqlite_schema
             WHERE type = 'table' AND name IN ('photo_groups', 'photo_group_members')",
            [],
            |row| row.get(0),
        )
        .expect("read distinct-photo relationship tables");
    assert_eq!(photo_relationship_tables, 2);
}

#[test]
fn rejects_prior_identity_for_a_development_reset() {
    let mut connection = Connection::open_in_memory().expect("open prior revision fixture");
    configure_connection(&connection, false).expect("configure prior revision fixture");
    connection
        .execute_batch(
            "CREATE TABLE catalog_schema (
                 version INTEGER PRIMARY KEY NOT NULL CHECK (version = 1),
                 identity TEXT NOT NULL,
                 created_at_ms INTEGER NOT NULL
             ) STRICT;
             INSERT INTO catalog_schema(version, identity, created_at_ms)
             VALUES (1, 'shadow-catalog-v1-r24-preview-provenance', 1);",
        )
        .expect("seed prior v1 identity");

    assert!(matches!(
        initialize(&mut connection),
        Err(CatalogError::DevelopmentCatalogResetRequired { found: Some(1) })
    ));
    assert!(table_exists(&connection, "catalog_schema").expect("preserve reset marker"));
}

#[test]
fn realigns_exact_previous_dated_metadata_without_touching_catalog_data() {
    let mut catalog = Catalog::open_in_memory().expect("open current catalog");
    catalog
        .connection
        .execute(
            "INSERT INTO photos(id, created_at_ms) VALUES (zeroblob(16), 7)",
            [],
        )
        .expect("persist photo before metadata alignment");
    catalog
        .connection
        .execute_batch(
            "ALTER TABLE catalog_schema RENAME TO catalog_schema_current;
             CREATE TABLE catalog_schema (
                 version INTEGER PRIMARY KEY NOT NULL CHECK (version = 2026080902),
                 identity TEXT NOT NULL CHECK (identity = 'shadow-catalog-20260809.2-photo-relationships'),
                 created_at_ms INTEGER NOT NULL
             ) STRICT;
             INSERT INTO catalog_schema(version, identity, created_at_ms)
             VALUES (2026080902, 'shadow-catalog-20260809.2-photo-relationships', 123);
             DROP TABLE catalog_schema_current;",
        )
        .expect("seed exact equivalent metadata");

    initialize(&mut catalog.connection).expect("realign equivalent dated metadata");

    assert_eq!(
        current_version(&catalog.connection).expect("current revision"),
        SCHEMA_VERSION
    );
    let created_at_ms: i64 = catalog
        .connection
        .query_row("SELECT created_at_ms FROM catalog_schema", [], |row| {
            row.get(0)
        })
        .expect("preserve metadata timestamp");
    assert_eq!(created_at_ms, 123);
    let photos: i64 = catalog
        .connection
        .query_row("SELECT COUNT(*) FROM photos", [], |row| row.get(0))
        .expect("preserve catalog data");
    assert_eq!(photos, 1);
}

#[test]
fn rejects_legacy_catalog_without_a_migration_attempt() {
    let mut connection = Connection::open_in_memory().expect("open legacy fixture");
    configure_connection(&connection, false).expect("configure legacy fixture");
    connection
        .execute_batch(
            "CREATE TABLE schema_migrations (
                 version INTEGER PRIMARY KEY NOT NULL,
                 applied_at_ms INTEGER NOT NULL
             ) STRICT;",
        )
        .expect("create legacy schema marker");
    connection
        .execute(
            "INSERT INTO schema_migrations(version, applied_at_ms) VALUES (14, 1)",
            [],
        )
        .expect("record legacy schema version");

    assert!(matches!(
        initialize(&mut connection),
        Err(CatalogError::DevelopmentCatalogResetRequired { found: Some(14) })
    ));
    assert!(table_exists(&connection, "schema_migrations").expect("legacy marker remains"));
    assert!(!table_exists(&connection, "catalog_schema").expect("no partial v1 state"));
}
