//! SQLite-backed catalog persistence.
//!
//! This crate owns schema migration and write transactions. It deliberately
//! knows nothing about Qt, RAW decoding, or render jobs.

mod cache_artifact;
mod decode_snapshot;
mod import_journal;
mod store;
mod writer;

use std::{path::Path, time::Duration};

use rusqlite::{Connection, OptionalExtension, Transaction, params, types::Type};
use shadow_domain::{
    AssetLocation, EntityId, LocationId, LocationStatus, PhotoId, RepresentationId,
    RepresentationKind,
};
use thiserror::Error;
use uuid::Uuid;

pub use cache_artifact::{
    CachedArtifact, CachedArtifactRecord, CachedArtifactRole, RecordCachedArtifact,
    RecordCachedArtifactStatus,
};
pub use decode_snapshot::{
    DecodeSnapshotRecord, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
    RepresentationFingerprint,
};
pub use import_journal::{ImportSession, ImportSessionState, ImportSessionSummary};
pub use store::CatalogStore;
pub use writer::{CatalogActor, CatalogHandle};

const SCHEMA_VERSION: i64 = 4;

const MIGRATION_V1: &str = r"
CREATE TABLE photos (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    created_at_ms   INTEGER NOT NULL,
    lifecycle_state TEXT NOT NULL DEFAULT 'active'
        CHECK (lifecycle_state IN ('active', 'archived', 'trashed'))
) STRICT;

CREATE TABLE representations (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    photo_id        BLOB NOT NULL CHECK (length(photo_id) = 16),
    kind            TEXT NOT NULL,
    byte_len        INTEGER NOT NULL CHECK (byte_len >= 0),
    modified_at_ms  INTEGER,
    created_at_ms   INTEGER NOT NULL,
    content_hash    BLOB,
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX representations_photo_id_idx ON representations(photo_id);

CREATE TABLE locations (
    id                BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    representation_id BLOB NOT NULL CHECK (length(representation_id) = 16),
    platform          TEXT NOT NULL,
    native_path       BLOB NOT NULL,
    display_path      TEXT NOT NULL,
    status            TEXT NOT NULL
        CHECK (status IN ('online', 'offline', 'needs_revalidation')),
    created_at_ms     INTEGER NOT NULL,
    UNIQUE (platform, native_path),
    FOREIGN KEY (representation_id) REFERENCES representations(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX locations_representation_id_idx ON locations(representation_id);
CREATE INDEX locations_status_idx ON locations(status);
";

const MIGRATION_V2: &str = r"
CREATE TABLE import_sessions (
    id                BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    root_platform     TEXT NOT NULL,
    root_native_path  BLOB NOT NULL,
    root_display_path TEXT NOT NULL,
    state             TEXT NOT NULL
        CHECK (state IN ('running', 'completed', 'failed', 'cancelled')),
    started_at_ms     INTEGER NOT NULL,
    updated_at_ms     INTEGER NOT NULL,
    finished_at_ms    INTEGER,
    last_error        TEXT
) STRICT;

CREATE INDEX import_sessions_state_idx ON import_sessions(state, updated_at_ms);

CREATE TABLE import_entries (
    session_id        BLOB NOT NULL CHECK (length(session_id) = 16),
    native_path       BLOB NOT NULL,
    display_path      TEXT NOT NULL,
    kind              TEXT NOT NULL,
    byte_len          INTEGER NOT NULL CHECK (byte_len >= 0),
    modified_at_ms    INTEGER,
    state             TEXT NOT NULL
        CHECK (state IN ('discovered', 'inserted', 'unchanged', 'needs_revalidation', 'failed')),
    photo_id          BLOB CHECK (photo_id IS NULL OR length(photo_id) = 16),
    representation_id BLOB CHECK (representation_id IS NULL OR length(representation_id) = 16),
    location_id       BLOB CHECK (location_id IS NULL OR length(location_id) = 16),
    error             TEXT,
    updated_at_ms     INTEGER NOT NULL,
    PRIMARY KEY (session_id, native_path),
    FOREIGN KEY (session_id) REFERENCES import_sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE RESTRICT,
    FOREIGN KEY (representation_id) REFERENCES representations(id) ON DELETE RESTRICT,
    FOREIGN KEY (location_id) REFERENCES locations(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX import_entries_state_idx ON import_entries(session_id, state);

CREATE TABLE import_issues (
    id            INTEGER PRIMARY KEY,
    session_id    BLOB NOT NULL CHECK (length(session_id) = 16),
    native_path   BLOB NOT NULL,
    display_path  TEXT NOT NULL,
    message       TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL,
    FOREIGN KEY (session_id) REFERENCES import_sessions(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX import_issues_session_idx ON import_issues(session_id);
";

const MIGRATION_V3: &str = r"
CREATE TABLE representation_decode_snapshots (
    representation_id       BLOB NOT NULL CHECK (length(representation_id) = 16),
    provider_id              TEXT NOT NULL CHECK (length(provider_id) > 0),
    provider_version         TEXT NOT NULL,
    snapshot_schema          INTEGER NOT NULL CHECK (snapshot_schema = 1),
    snapshot_json            TEXT NOT NULL CHECK (json_valid(snapshot_json)),
    source_byte_len          INTEGER NOT NULL CHECK (source_byte_len >= 0),
    source_modified_at_ms    INTEGER,
    inspected_at_ms          INTEGER NOT NULL,
    has_metadata             INTEGER NOT NULL CHECK (has_metadata IN (0, 1)),
    has_embedded_previews    INTEGER NOT NULL CHECK (has_embedded_previews IN (0, 1)),
    can_decode_mosaic        INTEGER NOT NULL CHECK (can_decode_mosaic IN (0, 1)),
    can_render_reference_rgb INTEGER NOT NULL CHECK (can_render_reference_rgb IN (0, 1)),
    has_pending_corrections  INTEGER NOT NULL CHECK (has_pending_corrections IN (0, 1)),
    PRIMARY KEY (representation_id, provider_id),
    FOREIGN KEY (representation_id) REFERENCES representations(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX representation_decode_capability_idx
    ON representation_decode_snapshots(can_decode_mosaic, has_embedded_previews);

CREATE TABLE representation_previews (
    representation_id   BLOB NOT NULL CHECK (length(representation_id) = 16),
    provider_id          TEXT NOT NULL,
    provider_preview_id  INTEGER NOT NULL CHECK (provider_preview_id >= 0),
    codec                TEXT NOT NULL
        CHECK (codec IN ('unknown', 'jpeg', 'bitmap', 'jpeg_xl', 'h265')),
    width                INTEGER NOT NULL CHECK (width >= 0),
    height               INTEGER NOT NULL CHECK (height >= 0),
    bits_per_channel     INTEGER NOT NULL CHECK (bits_per_channel >= 0),
    channels             INTEGER NOT NULL CHECK (channels >= 0),
    encoded_bytes        INTEGER NOT NULL CHECK (encoded_bytes >= 0),
    decodable            INTEGER NOT NULL CHECK (decodable IN (0, 1)),
    PRIMARY KEY (representation_id, provider_id, provider_preview_id),
    FOREIGN KEY (representation_id, provider_id)
        REFERENCES representation_decode_snapshots(representation_id, provider_id)
        ON DELETE CASCADE
) STRICT;

CREATE INDEX representation_previews_selection_idx
    ON representation_previews(representation_id, decodable, width, height);
";

const MIGRATION_V4: &str = r"
CREATE TABLE representation_cached_artifacts (
    representation_id    BLOB NOT NULL CHECK (length(representation_id) = 16),
    role                 TEXT NOT NULL
        CHECK (role IN ('embedded_preview', 'generated_proxy')),
    variant_key          TEXT NOT NULL CHECK (length(variant_key) > 0),
    generator_id         TEXT NOT NULL CHECK (length(generator_id) > 0),
    generator_version    TEXT NOT NULL,
    provider_preview_id  INTEGER CHECK (provider_preview_id IS NULL OR provider_preview_id >= 0),
    source_byte_len      INTEGER NOT NULL CHECK (source_byte_len >= 0),
    source_modified_at_ms INTEGER,
    blob_algorithm       TEXT NOT NULL CHECK (length(blob_algorithm) > 0),
    blob_digest          BLOB NOT NULL CHECK (length(blob_digest) = 32),
    blob_byte_len        INTEGER NOT NULL CHECK (blob_byte_len >= 0),
    codec                TEXT NOT NULL
        CHECK (codec IN ('unknown', 'jpeg', 'bitmap', 'jpeg_xl', 'h265')),
    byte_order           TEXT NOT NULL
        CHECK (byte_order IN ('not_applicable', 'native', 'little_endian', 'big_endian')),
    width                INTEGER NOT NULL CHECK (width >= 0),
    height               INTEGER NOT NULL CHECK (height >= 0),
    bits_per_channel     INTEGER NOT NULL CHECK (bits_per_channel >= 0),
    channels             INTEGER NOT NULL CHECK (channels >= 0),
    created_at_ms        INTEGER NOT NULL,
    PRIMARY KEY (representation_id, role, variant_key),
    FOREIGN KEY (representation_id) REFERENCES representations(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX representation_cached_artifact_lookup_idx
    ON representation_cached_artifacts(representation_id, role, width, height);
CREATE INDEX representation_cached_artifact_blob_idx
    ON representation_cached_artifacts(blob_algorithm, blob_digest);
";

#[derive(Debug, Error)]
pub enum CatalogError {
    #[error("SQLite catalog error: {0}")]
    Sqlite(#[from] rusqlite::Error),
    #[error("import session {0} does not exist")]
    ImportSessionNotFound(shadow_domain::ImportSessionId),
    #[error("import session {id} cannot be used while state is {state}")]
    InvalidImportSessionState {
        id: shadow_domain::ImportSessionId,
        state: &'static str,
    },
    #[error("import entry is missing in session {session_id}: {display_path}")]
    ImportEntryNotFound {
        session_id: shadow_domain::ImportSessionId,
        display_path: String,
    },
    #[error("cannot start catalog writer actor: {0}")]
    ActorStart(#[source] std::io::Error),
    #[error("catalog writer actor is unavailable")]
    ActorUnavailable,
    #[error("catalog writer actor panicked")]
    ActorPanicked,
    #[error("representation {0} does not exist")]
    RepresentationNotFound(RepresentationId),
    #[error("invalid decode snapshot: {0}")]
    InvalidDecodeSnapshot(&'static str),
    #[error("decode snapshot field {field} is outside SQLite's integer range")]
    DecodeSnapshotValueOutOfRange { field: &'static str },
    #[error("unsupported persisted decode snapshot schema {0}")]
    UnsupportedDecodeSnapshotSchema(i64),
    #[error("decode snapshot JSON error: {0}")]
    DecodeSnapshotJson(#[from] serde_json::Error),
    #[error("invalid cached artifact: {0}")]
    InvalidCachedArtifact(&'static str),
    #[error("cached artifact field {field} is outside SQLite's integer range")]
    CachedArtifactValueOutOfRange { field: &'static str },
    #[error("unknown persisted cached artifact {field}: {value}")]
    UnknownCachedArtifactValue { field: &'static str, value: String },
}

#[derive(Debug)]
pub struct Catalog {
    connection: Connection,
}

#[derive(Debug, Clone)]
pub struct RegisterAsset {
    pub kind: RepresentationKind,
    pub location: AssetLocation,
    pub byte_len: u64,
    pub modified_at_ms: Option<i64>,
    pub now_ms: i64,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct RegisteredAsset {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub location_id: LocationId,
    pub status: RegistrationStatus,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RegistrationStatus {
    Inserted,
    Unchanged,
    NeedsRevalidation,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Default)]
pub struct CatalogStats {
    pub photos: u64,
    pub representations: u64,
    pub locations: u64,
    pub locations_needing_revalidation: u64,
}

#[derive(Debug)]
struct ExistingAsset {
    photo_id: PhotoId,
    representation_id: RepresentationId,
    location_id: LocationId,
    byte_len: u64,
    modified_at_ms: Option<i64>,
}

impl Catalog {
    /// Opens or creates a file-backed catalog and applies all migrations.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` cannot open, configure, or migrate
    /// the catalog.
    pub fn open(path: &Path) -> Result<Self, CatalogError> {
        let mut connection = Connection::open(path)?;
        configure_connection(&connection, true)?;
        migrate(&mut connection)?;
        Ok(Self { connection })
    }

    /// Opens an isolated in-memory catalog, primarily for tests.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` cannot initialize or migrate the
    /// in-memory database.
    pub fn open_in_memory() -> Result<Self, CatalogError> {
        let mut connection = Connection::open_in_memory()?;
        configure_connection(&connection, false)?;
        migrate(&mut connection)?;
        Ok(Self { connection })
    }

    /// Returns the catalog schema version after migration.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the migration table cannot be queried.
    pub fn schema_version(&self) -> Result<i64, CatalogError> {
        current_schema_version(&self.connection).map_err(Into::into)
    }

    /// Registers an asset path atomically and idempotently.
    ///
    /// A path whose size or modification time changed is marked for later
    /// content revalidation instead of silently replacing its representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the registration transaction cannot be
    /// queried, written, or committed.
    pub fn register_asset(
        &mut self,
        request: &RegisterAsset,
    ) -> Result<RegisteredAsset, CatalogError> {
        let transaction = self.connection.transaction()?;
        let result = register_asset_in_transaction(&transaction, request)?;
        transaction.commit()?;
        Ok(result)
    }

    /// Returns persisted entity counts and the revalidation backlog.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if any count query fails or returns an invalid
    /// negative value.
    pub fn stats(&self) -> Result<CatalogStats, CatalogError> {
        let locations_needing_revalidation = self.connection.query_row(
            "SELECT COUNT(*) FROM locations WHERE status = ?1",
            [LocationStatus::NeedsRevalidation.as_str()],
            |row| row.get(0),
        )?;

        Ok(CatalogStats {
            photos: count_rows(&self.connection, "photos")?,
            representations: count_rows(&self.connection, "representations")?,
            locations: count_rows(&self.connection, "locations")?,
            locations_needing_revalidation: non_negative_count(locations_needing_revalidation)?,
        })
    }
}

fn register_asset_in_transaction(
    transaction: &Transaction<'_>,
    request: &RegisterAsset,
) -> rusqlite::Result<RegisteredAsset> {
    let existing = find_existing_asset(transaction, &request.location)?;

    if let Some(existing) = existing {
        let unchanged = existing.byte_len == request.byte_len
            && existing.modified_at_ms == request.modified_at_ms;

        if !unchanged {
            transaction.execute(
                "UPDATE locations SET status = ?1 WHERE id = ?2",
                params![
                    LocationStatus::NeedsRevalidation.as_str(),
                    existing.location_id.as_bytes().as_slice()
                ],
            )?;
        }

        Ok(RegisteredAsset {
            photo_id: existing.photo_id,
            representation_id: existing.representation_id,
            location_id: existing.location_id,
            status: if unchanged {
                RegistrationStatus::Unchanged
            } else {
                RegistrationStatus::NeedsRevalidation
            },
        })
    } else {
        insert_asset(transaction, request)
    }
}

fn configure_connection(connection: &Connection, file_backed: bool) -> rusqlite::Result<()> {
    connection.busy_timeout(Duration::from_secs(5))?;
    connection.execute_batch(
        "PRAGMA foreign_keys = ON;
         PRAGMA temp_store = MEMORY;",
    )?;

    if file_backed {
        connection.execute_batch(
            "PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;",
        )?;
    }

    Ok(())
}

fn migrate(connection: &mut Connection) -> rusqlite::Result<()> {
    connection.execute_batch(
        "CREATE TABLE IF NOT EXISTS schema_migrations (
             version       INTEGER PRIMARY KEY NOT NULL,
             applied_at_ms INTEGER NOT NULL
         ) STRICT;",
    )?;

    let version = current_schema_version(connection)?;
    if version < 1 {
        let transaction = connection.transaction()?;
        transaction.execute_batch(MIGRATION_V1)?;
        transaction.execute(
            "INSERT INTO schema_migrations(version, applied_at_ms) VALUES (?1, unixepoch('subsec') * 1000)",
            [1_i64],
        )?;
        transaction.commit()?;
    }

    let version = current_schema_version(connection)?;
    if version < 2 {
        let transaction = connection.transaction()?;
        transaction.execute_batch(MIGRATION_V2)?;
        transaction.execute(
            "INSERT INTO schema_migrations(version, applied_at_ms) VALUES (?1, unixepoch('subsec') * 1000)",
            [2_i64],
        )?;
        transaction.commit()?;
    }

    let version = current_schema_version(connection)?;
    if version < 3 {
        let transaction = connection.transaction()?;
        transaction.execute_batch(MIGRATION_V3)?;
        transaction.execute(
            "INSERT INTO schema_migrations(version, applied_at_ms) VALUES (?1, unixepoch('subsec') * 1000)",
            [3_i64],
        )?;
        transaction.commit()?;
    }

    let version = current_schema_version(connection)?;
    if version < 4 {
        let transaction = connection.transaction()?;
        transaction.execute_batch(MIGRATION_V4)?;
        transaction.execute(
            "INSERT INTO schema_migrations(version, applied_at_ms) VALUES (?1, unixepoch('subsec') * 1000)",
            [4_i64],
        )?;
        transaction.commit()?;
    }

    let final_version = current_schema_version(connection)?;
    if final_version != SCHEMA_VERSION {
        return Err(rusqlite::Error::InvalidQuery);
    }

    Ok(())
}

fn current_schema_version(connection: &Connection) -> rusqlite::Result<i64> {
    connection.query_row(
        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations",
        [],
        |row| row.get(0),
    )
}

fn find_existing_asset(
    transaction: &Transaction<'_>,
    location: &AssetLocation,
) -> rusqlite::Result<Option<ExistingAsset>> {
    transaction
        .query_row(
            "SELECT p.id, r.id, l.id, r.byte_len, r.modified_at_ms
             FROM locations l
             JOIN representations r ON r.id = l.representation_id
             JOIN photos p ON p.id = r.photo_id
             WHERE l.platform = ?1 AND l.native_path = ?2",
            params![location.platform.as_str(), location.native_path],
            |row| {
                let byte_len: i64 = row.get(3)?;
                Ok(ExistingAsset {
                    photo_id: read_id(row, 0)?,
                    representation_id: read_id(row, 1)?,
                    location_id: read_id(row, 2)?,
                    byte_len: u64::try_from(byte_len).map_err(|error| {
                        rusqlite::Error::FromSqlConversionFailure(3, Type::Integer, Box::new(error))
                    })?,
                    modified_at_ms: row.get(4)?,
                })
            },
        )
        .optional()
}

fn insert_asset(
    transaction: &Transaction<'_>,
    request: &RegisterAsset,
) -> rusqlite::Result<RegisteredAsset> {
    let photo_id = PhotoId::new_v7();
    let representation_id = RepresentationId::new_v7();
    let location_id = LocationId::new_v7();
    let byte_len = i64::try_from(request.byte_len)
        .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))?;

    transaction.execute(
        "INSERT INTO photos(id, created_at_ms) VALUES (?1, ?2)",
        params![photo_id.as_bytes().as_slice(), request.now_ms],
    )?;
    transaction.execute(
        "INSERT INTO representations(
             id, photo_id, kind, byte_len, modified_at_ms, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            representation_id.as_bytes().as_slice(),
            photo_id.as_bytes().as_slice(),
            request.kind.as_str(),
            byte_len,
            request.modified_at_ms,
            request.now_ms
        ],
    )?;
    transaction.execute(
        "INSERT INTO locations(
             id, representation_id, platform, native_path, display_path, status, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        params![
            location_id.as_bytes().as_slice(),
            representation_id.as_bytes().as_slice(),
            request.location.platform.as_str(),
            request.location.native_path,
            request.location.display_path,
            LocationStatus::Online.as_str(),
            request.now_ms
        ],
    )?;

    Ok(RegisteredAsset {
        photo_id,
        representation_id,
        location_id,
        status: RegistrationStatus::Inserted,
    })
}

fn read_id<I: EntityId>(row: &rusqlite::Row<'_>, index: usize) -> rusqlite::Result<I> {
    let bytes: Vec<u8> = row.get(index)?;
    let uuid = Uuid::from_slice(&bytes).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(index, Type::Blob, Box::new(error))
    })?;
    Ok(I::from_uuid(uuid))
}

fn count_rows(connection: &Connection, table: &str) -> rusqlite::Result<u64> {
    let sql = match table {
        "photos" => "SELECT COUNT(*) FROM photos",
        "representations" => "SELECT COUNT(*) FROM representations",
        "locations" => "SELECT COUNT(*) FROM locations",
        _ => return Err(rusqlite::Error::InvalidQuery),
    };
    let count = connection.query_row(sql, [], |row| row.get(0))?;
    non_negative_count(count)
}

fn non_negative_count(count: i64) -> rusqlite::Result<u64> {
    u64::try_from(count).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(0, Type::Integer, Box::new(error))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use shadow_domain::Platform;

    fn request(byte_len: u64, modified_at_ms: Option<i64>) -> RegisterAsset {
        RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/DSC_0001.NEF".to_vec(),
                "/photos/DSC_0001.NEF",
            ),
            byte_len,
            modified_at_ms,
            now_ms: 1_700_000_000_000,
        }
    }

    #[test]
    fn migration_creates_current_schema() {
        let catalog = Catalog::open_in_memory().expect("open catalog");

        assert_eq!(catalog.schema_version().expect("schema version"), 4);
    }

    #[test]
    fn version_two_catalog_migrates_without_rebuilding_existing_tables() {
        let root = std::env::temp_dir().join(format!("shadow-catalog-v2-{}", PhotoId::new_v7()));
        std::fs::create_dir_all(&root).expect("create migration fixture");
        let path = root.join("catalog.sqlite");
        {
            let mut connection = Connection::open(&path).expect("open v2 fixture");
            configure_connection(&connection, false).expect("configure fixture");
            connection
                .execute_batch(
                    "CREATE TABLE schema_migrations (
                         version INTEGER PRIMARY KEY NOT NULL,
                         applied_at_ms INTEGER NOT NULL
                     ) STRICT;",
                )
                .expect("create migration table");
            let transaction = connection.transaction().expect("start v2 fixture");
            transaction.execute_batch(MIGRATION_V1).expect("apply v1");
            transaction
                .execute(
                    "INSERT INTO schema_migrations(version, applied_at_ms) VALUES (1, 1)",
                    [],
                )
                .expect("record v1");
            transaction.execute_batch(MIGRATION_V2).expect("apply v2");
            transaction
                .execute(
                    "INSERT INTO schema_migrations(version, applied_at_ms) VALUES (2, 2)",
                    [],
                )
                .expect("record v2");
            transaction.commit().expect("commit v2 fixture");
        }

        let catalog = Catalog::open(&path).expect("migrate v2 catalog");
        assert_eq!(catalog.schema_version().expect("schema version"), 4);
        let snapshot_tables: i64 = catalog
            .connection
            .query_row(
                "SELECT COUNT(*) FROM sqlite_schema
                 WHERE type = 'table' AND name IN (
                     'representation_decode_snapshots', 'representation_previews',
                     'representation_cached_artifacts'
                 )",
                [],
                |row| row.get(0),
            )
            .expect("query migrated tables");
        assert_eq!(snapshot_tables, 3);
        drop(catalog);
        std::fs::remove_dir_all(root).expect("remove migration fixture");
    }

    #[test]
    fn registering_the_same_location_is_idempotent() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let first = catalog
            .register_asset(&request(42, Some(100)))
            .expect("first registration");
        let second = catalog
            .register_asset(&request(42, Some(100)))
            .expect("second registration");

        assert_eq!(first.status, RegistrationStatus::Inserted);
        assert_eq!(second.status, RegistrationStatus::Unchanged);
        assert_eq!(first.photo_id, second.photo_id);
        assert_eq!(catalog.stats().expect("stats").photos, 1);
    }

    #[test]
    fn changed_file_is_marked_for_revalidation_without_silent_replacement() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        catalog
            .register_asset(&request(42, Some(100)))
            .expect("initial registration");
        let changed = catalog
            .register_asset(&request(84, Some(200)))
            .expect("changed registration");

        assert_eq!(changed.status, RegistrationStatus::NeedsRevalidation);
        assert_eq!(
            catalog
                .stats()
                .expect("stats")
                .locations_needing_revalidation,
            1
        );
    }
}
