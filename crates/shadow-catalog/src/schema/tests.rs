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
fn migrates_photo_variant_revision_without_replacing_existing_data() {
    let mut catalog = Catalog::open_in_memory().expect("open current catalog");
    catalog
        .connection
        .execute(
            "INSERT INTO photos(id, created_at_ms) VALUES (zeroblob(16), 7)",
            [],
        )
        .expect("persist photo before relationship migration");
    catalog
        .connection
        .execute_batch(
            "DROP TABLE photo_group_members;
             DROP TABLE photo_groups;",
        )
        .expect("remove relationship responsibility from fixture");
    install_schema_marker(
        &catalog.connection,
        PHOTO_VARIANTS_SCHEMA_VERSION,
        PHOTO_VARIANTS_SCHEMA_IDENTITY,
        123,
    );

    initialize(&mut catalog.connection).expect("migrate photo relationship schema");

    assert_eq!(
        current_version(&catalog.connection).expect("current revision"),
        SCHEMA_VERSION
    );
    assert_eq!(
        count_table_rows(&catalog.connection, "photos"),
        1,
        "migration preserves existing photos"
    );
    assert_eq!(
        count_table_rows(&catalog.connection, "photo_variants"),
        1,
        "migration preserves existing Variants"
    );
    assert!(table_exists(&catalog.connection, "photo_groups").expect("relationship table"));
    assert_eq!(schema_created_at_ms(&catalog.connection), 123);
}

#[test]
fn migrates_logical_photo_revision_and_backfills_working_variant_heads() {
    let mut catalog = Catalog::open_in_memory().expect("open current catalog");
    catalog
        .connection
        .execute_batch(
            "INSERT INTO photos(id, created_at_ms) VALUES (zeroblob(16), 7);
             INSERT INTO recipe_commits(
                 id, photo_id, recipe_id, commit_json, snapshot_digest, created_at_ms
             ) VALUES (
                 x'01010101010101010101010101010101', zeroblob(16),
                 x'02020202020202020202020202020202', '{}', zeroblob(32), 9
             );
             INSERT INTO recipe_refs(photo_id, name, kind, commit_id, updated_at_ms)
             VALUES (
                 zeroblob(16), 'working', 'working',
                 x'01010101010101010101010101010101', 11
             );
             DROP TABLE photo_group_members;
             DROP TABLE photo_groups;
             DROP TRIGGER photos_create_default_variant;
             DROP TABLE photo_variant_state;
             DROP TABLE photo_variants;",
        )
        .expect("seed logical-photo predecessor fixture");
    install_schema_marker(
        &catalog.connection,
        LOGICAL_PHOTOS_SCHEMA_VERSION,
        LOGICAL_PHOTOS_SCHEMA_IDENTITY,
        456,
    );

    initialize(&mut catalog.connection).expect("migrate logical-photo schema");

    let variant: (Vec<u8>, Option<Vec<u8>>, i64, i64) = catalog
        .connection
        .query_row(
            "SELECT id, head_commit_id, created_at_ms, updated_at_ms
             FROM photo_variants WHERE photo_id = zeroblob(16)",
            [],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
        )
        .expect("read migrated default Variant");
    assert_eq!(variant.0, vec![0; 16]);
    assert_eq!(variant.1, Some(vec![1; 16]));
    assert_eq!((variant.2, variant.3), (7, 11));
    assert_eq!(
        count_table_rows(&catalog.connection, "photo_variant_state"),
        1
    );
    assert!(table_exists(&catalog.connection, "photo_groups").expect("relationship table"));
    assert_eq!(schema_created_at_ms(&catalog.connection), 456);
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

fn install_schema_marker(
    connection: &Connection,
    version: i64,
    identity: &str,
    created_at_ms: i64,
) {
    connection
        .execute_batch(&format!(
            "ALTER TABLE catalog_schema RENAME TO catalog_schema_current;
             CREATE TABLE catalog_schema (
                 version INTEGER PRIMARY KEY NOT NULL CHECK (version = {version}),
                 identity TEXT NOT NULL CHECK (identity = '{identity}'),
                 created_at_ms INTEGER NOT NULL
             ) STRICT;"
        ))
        .expect("create predecessor schema marker");
    connection
        .execute(
            "INSERT INTO catalog_schema(version, identity, created_at_ms) VALUES (?1, ?2, ?3)",
            (version, identity, created_at_ms),
        )
        .expect("insert predecessor schema marker");
    connection
        .execute_batch("DROP TABLE catalog_schema_current;")
        .expect("remove current schema marker");
}

fn count_table_rows(connection: &Connection, table: &str) -> i64 {
    connection
        .query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |row| {
            row.get(0)
        })
        .expect("count fixture rows")
}

fn schema_created_at_ms(connection: &Connection) -> i64 {
    connection
        .query_row("SELECT created_at_ms FROM catalog_schema", [], |row| {
            row.get(0)
        })
        .expect("read schema creation time")
}
