//! SQLite-backed catalog persistence.
//!
//! This crate owns the current development schema and write transactions. It deliberately
//! knows nothing about Qt, RAW decoding, or render jobs.

mod backup;
mod cache_artifact;
mod decision;
mod decode_snapshot;
mod edit_repository;
mod export_queue;
mod feedback;
mod import_journal;
mod library;
mod library_metadata;
mod recipe;
mod review;
mod store;
mod technical_observation;
mod writer;

use std::{path::Path, time::Duration};

use rusqlite::{Connection, OptionalExtension, Transaction, params, types::Type};
use shadow_domain::{
    AssetLocation, EntityId, LocationId, LocationStatus, PhotoFlag, PhotoId, RepresentationId,
    RepresentationKind,
};
use thiserror::Error;
use uuid::Uuid;

pub use backup::{
    CatalogBackupError, CatalogBackupReceipt, CatalogBackupVerification, create_catalog_backup,
    verify_catalog_backup,
};
pub use cache_artifact::{
    CachedArtifact, CachedArtifactGeneratorIdentity, CachedArtifactRecord, CachedArtifactRole,
    InvalidateCachedArtifactStatus, LiveCachedArtifactBlob, RecordCachedArtifact,
    RecordCachedArtifactStatus,
};
pub use decision::{MAX_PHOTO_DECISION_PAGE_SIZE, PhotoDecisionPage};
pub use decode_snapshot::{
    DecodeSnapshotRecord, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
    RepresentationFingerprint,
};
pub use edit_repository::{
    CommitEditRepository, CommitRecipeAndEditRepository, CommitRecipeAndEditRepositoryResult,
    EditObjectPackWrite, EditObjectRecord, EditRepositoryCommitRecord, EditRepositoryRefRecord,
    EditRepositoryRefUpdate, StoreEditObjectPackResult,
};
pub use export_queue::{
    AdvanceExportItem, EnqueueExportJob, ExportFailure, ExportItemId, ExportItemRecord,
    ExportItemState, ExportJobId, ExportJobProgress, ExportJobRecord, ExportJobState,
    ExportOutputReceiptId, ExportOutputReceiptRecord, ExportPresetId, ExportPresetRecord,
    ExportPresetRevisionId, ExportPresetRevisionRecord, ExportQueueRecovery, ExportSettingsSource,
    MAX_EXPORT_JOB_PAGE_SIZE, NewExportItem, NewExportOutputReceipt,
};
pub use feedback::{FeedbackPage, MAX_FEEDBACK_PAGE_SIZE};
pub use import_journal::{
    ImportSession, ImportSessionState, ImportSessionSummary, SourceScanReconciliation,
};
pub use library::{
    AlbumKind, AlbumRecord, ContentIdentity, ContentIdentityScope, LibraryApertureRange,
    LibraryDateRange, LibraryPhotoCursor, LibraryPhotoFacts, LibraryPhotoFilter, LibraryPhotoPage,
    LibraryPhotoRecord, LibrarySourceRecord, MAX_LIBRARY_PAGE_SIZE, PhotoLibraryState,
    RecordRepresentationContentIdentity, RecordRepresentationContentIdentityStatus, RelinkMatch,
    SetPhotoLibraryState, library_equipment_key,
};
pub use recipe::{
    CommitRecipe, RecipeCommitRecord, RecipeRefExpectation, RecipeRefKind, RecipeRefRecord,
    RecipeRefTarget, SetRecipeRef,
};
pub use review::{ReviewCursor, ReviewItemRecord, ReviewPageRecord};
pub use store::CatalogStore;
pub use technical_observation::{
    RecordTechnicalObservation, RecordTechnicalObservationStatus, TechnicalObservationRecord,
    TechnicalObservationRevision, TechnicalObservationSummary,
};
pub use writer::{CatalogActor, CatalogHandle};

/// The catalog is intentionally unstable until the product reaches its first
/// compatibility promise. There is only one supported on-disk shape: a fresh
/// schema v1. Older development catalogs are rejected and must be reset rather
/// than carried forward through a migration chain.
const SCHEMA_VERSION: i64 = 1;

const SCHEMA_V1_CORE: &str = r"
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

const SCHEMA_V1_IMPORT: &str = r"
CREATE TABLE import_sessions (
    id                BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    root_platform     TEXT NOT NULL,
    root_native_path  BLOB NOT NULL,
    root_display_path TEXT NOT NULL,
    source_id         BLOB CHECK (source_id IS NULL OR length(source_id) = 16),
    state             TEXT NOT NULL
        CHECK (state IN ('running', 'completed', 'failed', 'cancelled')),
    started_at_ms     INTEGER NOT NULL,
    updated_at_ms     INTEGER NOT NULL,
    finished_at_ms    INTEGER,
    last_error        TEXT
) STRICT;

CREATE INDEX import_sessions_state_idx ON import_sessions(state, updated_at_ms);
CREATE INDEX import_sessions_source_idx ON import_sessions(source_id, updated_at_ms DESC);

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

const SCHEMA_V1_DECODER: &str = r"
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
    can_decode_raw_frame     INTEGER NOT NULL CHECK (can_decode_raw_frame IN (0, 1)),
    can_render_reference_rgb INTEGER NOT NULL CHECK (can_render_reference_rgb IN (0, 1)),
    has_pending_corrections  INTEGER NOT NULL CHECK (has_pending_corrections IN (0, 1)),
    PRIMARY KEY (representation_id, provider_id),
    FOREIGN KEY (representation_id) REFERENCES representations(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX representation_decode_capability_idx
    ON representation_decode_snapshots(can_decode_raw_frame, has_embedded_previews);

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

const SCHEMA_V1_CACHE: &str = r"
CREATE TABLE representation_cached_artifacts (
    representation_id    BLOB NOT NULL CHECK (length(representation_id) = 16),
    role                 TEXT NOT NULL
        CHECK (role IN ('recipe_preview', 'embedded_preview', 'generated_proxy')),
    variant_key          TEXT NOT NULL CHECK (length(variant_key) > 0),
    generator_id         TEXT NOT NULL CHECK (length(generator_id) > 0),
    generator_version    TEXT NOT NULL,
    recipe_snapshot_digest BLOB
        CHECK (recipe_snapshot_digest IS NULL OR length(recipe_snapshot_digest) = 32),
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

const SCHEMA_V1_RECIPE: &str = r"
CREATE TABLE recipe_commits (
    id             BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    photo_id       BLOB NOT NULL CHECK (length(photo_id) = 16),
    recipe_id      BLOB NOT NULL CHECK (length(recipe_id) = 16),
    commit_json    TEXT NOT NULL CHECK (json_valid(commit_json)),
    snapshot_digest BLOB NOT NULL CHECK (length(snapshot_digest) = 32),
    created_at_ms  INTEGER NOT NULL,
    UNIQUE (id, photo_id),
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX recipe_commits_photo_time_idx
    ON recipe_commits(photo_id, created_at_ms DESC, id);
CREATE INDEX recipe_commits_recipe_idx ON recipe_commits(recipe_id);
CREATE INDEX recipe_commits_digest_idx ON recipe_commits(snapshot_digest);

CREATE TABLE recipe_commit_parents (
    commit_id  BLOB NOT NULL CHECK (length(commit_id) = 16),
    parent_id  BLOB NOT NULL CHECK (length(parent_id) = 16),
    photo_id   BLOB NOT NULL CHECK (length(photo_id) = 16),
    position   INTEGER NOT NULL CHECK (position >= 0),
    PRIMARY KEY (commit_id, position),
    UNIQUE (commit_id, parent_id),
    FOREIGN KEY (commit_id, photo_id)
        REFERENCES recipe_commits(id, photo_id) ON DELETE CASCADE,
    FOREIGN KEY (parent_id, photo_id)
        REFERENCES recipe_commits(id, photo_id) ON DELETE RESTRICT
) STRICT;

CREATE TABLE recipe_refs (
    photo_id      BLOB NOT NULL CHECK (length(photo_id) = 16),
    name          TEXT NOT NULL CHECK (length(name) > 0),
    kind          TEXT NOT NULL
        CHECK (kind IN ('working', 'branch', 'named_version', 'tag')),
    commit_id     BLOB NOT NULL CHECK (length(commit_id) = 16),
    updated_at_ms INTEGER NOT NULL,
    PRIMARY KEY (photo_id, name),
    FOREIGN KEY (commit_id, photo_id)
        REFERENCES recipe_commits(id, photo_id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX recipe_refs_commit_idx ON recipe_refs(commit_id);
";

const SCHEMA_V1_FEEDBACK: &str = r"
CREATE TABLE ai_feedback_events (
    sequence       INTEGER PRIMARY KEY NOT NULL CHECK (sequence > 0),
    event_id       TEXT NOT NULL UNIQUE
        CHECK (length(event_id) BETWEEN 1 AND 256),
    occurred_at_ms INTEGER NOT NULL,
    scope_kind     TEXT NOT NULL CHECK (scope_kind IN ('global', 'project')),
    project_id     TEXT,
    event_json     TEXT NOT NULL CHECK (json_valid(event_json)),
    event_digest   BLOB NOT NULL CHECK (length(event_digest) = 32),
    CHECK (
        (scope_kind = 'global' AND project_id IS NULL) OR
        (scope_kind = 'project' AND project_id IS NOT NULL
            AND length(project_id) BETWEEN 1 AND 256)
    )
) STRICT;

CREATE INDEX ai_feedback_events_scope_sequence_idx
    ON ai_feedback_events(scope_kind, project_id, sequence);
CREATE INDEX ai_feedback_events_occurred_idx
    ON ai_feedback_events(occurred_at_ms, sequence);

CREATE TRIGGER ai_feedback_events_no_update
BEFORE UPDATE ON ai_feedback_events
BEGIN
    SELECT RAISE(ABORT, 'AI feedback events are append-only');
END;

CREATE TRIGGER ai_feedback_events_no_delete
BEFORE DELETE ON ai_feedback_events
BEGIN
    SELECT RAISE(ABORT, 'AI feedback events are append-only');
END;

CREATE TABLE ai_feedback_forget_facts (
    sequence           INTEGER PRIMARY KEY NOT NULL CHECK (sequence > 0),
    fact_id            TEXT NOT NULL UNIQUE
        CHECK (length(fact_id) BETWEEN 1 AND 256),
    target_event_id    TEXT NOT NULL,
    occurred_at_ms     INTEGER NOT NULL,
    fact_json          TEXT NOT NULL CHECK (json_valid(fact_json)),
    fact_digest        BLOB NOT NULL CHECK (length(fact_digest) = 32),
    FOREIGN KEY (target_event_id) REFERENCES ai_feedback_events(event_id)
        ON DELETE RESTRICT
) STRICT;

CREATE INDEX ai_feedback_forget_target_idx
    ON ai_feedback_forget_facts(target_event_id, sequence);

CREATE TRIGGER ai_feedback_forget_facts_no_update
BEFORE UPDATE ON ai_feedback_forget_facts
BEGIN
    SELECT RAISE(ABORT, 'AI feedback forget facts are append-only');
END;

CREATE TRIGGER ai_feedback_forget_facts_no_delete
BEFORE DELETE ON ai_feedback_forget_facts
BEGIN
    SELECT RAISE(ABORT, 'AI feedback forget facts are append-only');
END;
";

const SCHEMA_V1_TECHNICAL_OBSERVATION: &str = r"
CREATE TABLE representation_technical_observations (
    representation_id             BLOB NOT NULL CHECK (length(representation_id) = 16),
    source_role                   TEXT NOT NULL
        CHECK (source_role IN ('embedded_preview', 'generated_proxy')),
    source_variant_key            TEXT NOT NULL CHECK (length(source_variant_key) > 0),
    source_generator_id           TEXT NOT NULL CHECK (length(source_generator_id) > 0),
    source_generator_version      TEXT NOT NULL,
    source_provider_preview_id    INTEGER NOT NULL CHECK (source_provider_preview_id >= -1),
    source_blob_algorithm         TEXT NOT NULL CHECK (length(source_blob_algorithm) > 0),
    source_blob_digest            BLOB NOT NULL CHECK (length(source_blob_digest) = 32),
    source_blob_byte_len          INTEGER NOT NULL CHECK (source_blob_byte_len > 0),
    source_codec                  TEXT NOT NULL
        CHECK (source_codec IN ('unknown', 'jpeg', 'bitmap', 'jpeg_xl', 'h265')),
    source_byte_order             TEXT NOT NULL
        CHECK (source_byte_order IN ('not_applicable', 'native', 'little_endian', 'big_endian')),
    source_width                  INTEGER NOT NULL CHECK (source_width >= 0),
    source_height                 INTEGER NOT NULL CHECK (source_height >= 0),
    source_bits_per_channel       INTEGER NOT NULL CHECK (source_bits_per_channel >= 0),
    source_channels               INTEGER NOT NULL CHECK (source_channels >= 0),
    source_created_at_ms          INTEGER NOT NULL,
    source_byte_len               INTEGER NOT NULL CHECK (source_byte_len >= 0),
    source_modified_at_ms         INTEGER,
    observation_schema            INTEGER NOT NULL CHECK (observation_schema > 0),
    implementation_version        TEXT NOT NULL CHECK (length(implementation_version) > 0),
    display_luma_contract_version INTEGER NOT NULL CHECK (display_luma_contract_version > 0),
    preprocessing_version         TEXT NOT NULL CHECK (length(preprocessing_version) > 0),
    observation_json              TEXT NOT NULL CHECK (json_valid(observation_json)),
    observation_digest            BLOB NOT NULL CHECK (length(observation_digest) = 32),
    observed_at_ms                INTEGER NOT NULL,
    PRIMARY KEY (
        representation_id, source_role, source_variant_key, source_generator_id,
        source_generator_version, source_provider_preview_id, source_blob_algorithm,
        source_blob_digest, source_blob_byte_len, source_codec, source_byte_order,
        source_width, source_height, source_bits_per_channel, source_channels,
        observation_schema,
        implementation_version, display_luma_contract_version, preprocessing_version
    ),
    FOREIGN KEY (representation_id) REFERENCES representations(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX representation_technical_observation_source_idx
    ON representation_technical_observations(
        representation_id, source_role, source_variant_key,
        source_blob_algorithm, source_blob_digest
    );
";

// The current table is deliberately only a movable pointer. Flag/rating values
// remain authoritative in immutable, integrity-checked ledger events.
const SCHEMA_V1_DECISION: &str = r"
CREATE TABLE photo_decision_events (
    sequence             INTEGER PRIMARY KEY NOT NULL CHECK (sequence > 0),
    event_id             TEXT NOT NULL UNIQUE
        CHECK (length(event_id) BETWEEN 1 AND 256),
    photo_id             BLOB NOT NULL CHECK (length(photo_id) = 16),
    occurred_at_ms       INTEGER NOT NULL,
    origin               TEXT NOT NULL CHECK (origin = 'human'),
    before_head_sequence INTEGER NOT NULL CHECK (before_head_sequence >= 0),
    before_flag          TEXT NOT NULL
        CHECK (before_flag IN ('unflagged', 'picked', 'rejected')),
    before_rating        INTEGER NOT NULL CHECK (before_rating BETWEEN 0 AND 5),
    after_flag           TEXT NOT NULL
        CHECK (after_flag IN ('unflagged', 'picked', 'rejected')),
    after_rating         INTEGER NOT NULL CHECK (after_rating BETWEEN 0 AND 5),
    event_json           TEXT NOT NULL CHECK (json_valid(event_json)),
    event_digest         BLOB NOT NULL CHECK (length(event_digest) = 32),
    UNIQUE (sequence, photo_id),
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX photo_decision_events_photo_sequence_idx
    ON photo_decision_events(photo_id, sequence);

CREATE TRIGGER photo_decision_events_no_update
BEFORE UPDATE ON photo_decision_events
BEGIN
    SELECT RAISE(ABORT, 'photo decision events are append-only');
END;

CREATE TRIGGER photo_decision_events_no_delete
BEFORE DELETE ON photo_decision_events
BEGIN
    SELECT RAISE(ABORT, 'photo decision events are append-only');
END;

CREATE TABLE photo_decision_current (
    photo_id      BLOB PRIMARY KEY NOT NULL CHECK (length(photo_id) = 16),
    head_sequence INTEGER NOT NULL CHECK (head_sequence > 0),
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE CASCADE,
    FOREIGN KEY (head_sequence, photo_id)
        REFERENCES photo_decision_events(sequence, photo_id) ON DELETE RESTRICT
) STRICT;

CREATE TRIGGER photo_decision_current_only_forward
BEFORE UPDATE OF head_sequence ON photo_decision_current
WHEN NEW.head_sequence <= OLD.head_sequence
BEGIN
    SELECT RAISE(ABORT, 'photo decision head must move forward');
END;
";

// Library-level edit history is deliberately parallel to the per-photo Recipe
// tables. These immutable content-addressed objects, global commits, ordered
// parents, and CAS-updated refs are all part of the initial catalog shape.
const SCHEMA_V1_EDIT_REPOSITORY: &str = r"
CREATE TABLE edit_objects (
    digest          BLOB PRIMARY KEY NOT NULL CHECK (length(digest) = 32),
    hash_algorithm  TEXT NOT NULL CHECK (hash_algorithm = 'blake3-256'),
    kind            TEXT NOT NULL CHECK (length(kind) > 0),
    format_version  INTEGER NOT NULL CHECK (format_version > 0),
    payload_codec   TEXT NOT NULL CHECK (payload_codec = 'canonical-json'),
    payload         TEXT NOT NULL CHECK (json_valid(payload)),
    created_at_ms   INTEGER NOT NULL
) STRICT;

CREATE INDEX edit_objects_kind_idx
    ON edit_objects(kind, format_version, created_at_ms);

CREATE TRIGGER edit_objects_no_update
BEFORE UPDATE ON edit_objects
BEGIN
    SELECT RAISE(ABORT, 'edit objects are immutable');
END;

CREATE TABLE edit_object_edges (
    source_digest BLOB NOT NULL CHECK (length(source_digest) = 32),
    role          TEXT NOT NULL CHECK (length(role) > 0),
    position      INTEGER NOT NULL CHECK (position >= 0),
    target_digest BLOB NOT NULL CHECK (length(target_digest) = 32),
    PRIMARY KEY (source_digest, role, position),
    FOREIGN KEY (source_digest) REFERENCES edit_objects(digest) ON DELETE CASCADE,
    FOREIGN KEY (target_digest) REFERENCES edit_objects(digest) ON DELETE RESTRICT
) STRICT;

CREATE INDEX edit_object_edges_target_idx ON edit_object_edges(target_digest);

CREATE TRIGGER edit_object_edges_no_update
BEFORE UPDATE ON edit_object_edges
BEGIN
    SELECT RAISE(ABORT, 'edit object edges are immutable');
END;

CREATE TABLE edit_repository_commits (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 32),
    root_digest     BLOB NOT NULL CHECK (length(root_digest) = 32),
    format_version  INTEGER NOT NULL CHECK (format_version = 1),
    commit_json     TEXT NOT NULL CHECK (json_valid(commit_json)),
    created_at_ms   INTEGER NOT NULL,
    FOREIGN KEY (root_digest) REFERENCES edit_objects(digest) ON DELETE RESTRICT
) STRICT;

CREATE INDEX edit_repository_commits_root_idx ON edit_repository_commits(root_digest);
CREATE INDEX edit_repository_commits_time_idx
    ON edit_repository_commits(created_at_ms DESC, id);

CREATE TRIGGER edit_repository_commits_no_update
BEFORE UPDATE ON edit_repository_commits
BEGIN
    SELECT RAISE(ABORT, 'edit repository commits are immutable');
END;

CREATE TABLE edit_repository_commit_parents (
    commit_id BLOB NOT NULL CHECK (length(commit_id) = 32),
    parent_id BLOB NOT NULL CHECK (length(parent_id) = 32),
    position  INTEGER NOT NULL CHECK (position >= 0),
    PRIMARY KEY (commit_id, position),
    UNIQUE (commit_id, parent_id),
    FOREIGN KEY (commit_id) REFERENCES edit_repository_commits(id) ON DELETE CASCADE,
    FOREIGN KEY (parent_id) REFERENCES edit_repository_commits(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX edit_repository_commit_parents_parent_idx
    ON edit_repository_commit_parents(parent_id);

CREATE TABLE edit_repository_refs (
    name          TEXT PRIMARY KEY NOT NULL CHECK (length(name) BETWEEN 1 AND 256),
    kind          TEXT NOT NULL CHECK (kind IN ('branch', 'named_version', 'tag')),
    commit_id     BLOB NOT NULL CHECK (length(commit_id) = 32),
    updated_at_ms INTEGER NOT NULL,
    FOREIGN KEY (commit_id) REFERENCES edit_repository_commits(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX edit_repository_refs_commit_idx ON edit_repository_refs(commit_id);
";

// The photo-first Library read model treats directories as
// explicitly discovery sources and locations, never ownership boundaries for
// photos. The rows here are intentionally small, indexed projections; source
// decoder JSON and rebuildable preview bytes remain outside this hot path.
const SCHEMA_V1_LIBRARY: &str = r"
CREATE TABLE library_sources (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    platform        TEXT NOT NULL,
    native_path     BLOB NOT NULL,
    display_path    TEXT NOT NULL,
    enabled         INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
    created_at_ms   INTEGER NOT NULL,
    last_scanned_at_ms INTEGER,
    UNIQUE (platform, native_path)
) STRICT;

CREATE INDEX library_sources_enabled_scan_idx
    ON library_sources(enabled, last_scanned_at_ms DESC, id);

CREATE TABLE location_sources (
    location_id      BLOB NOT NULL CHECK (length(location_id) = 16),
    source_id        BLOB NOT NULL CHECK (length(source_id) = 16),
    first_seen_at_ms INTEGER NOT NULL,
    last_seen_at_ms  INTEGER NOT NULL,
    PRIMARY KEY (location_id, source_id),
    FOREIGN KEY (location_id) REFERENCES locations(id) ON DELETE CASCADE,
    FOREIGN KEY (source_id) REFERENCES library_sources(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX location_sources_source_seen_idx
    ON location_sources(source_id, last_seen_at_ms DESC, location_id);

CREATE TABLE representation_content_identities (
    representation_id BLOB NOT NULL CHECK (length(representation_id) = 16),
    scope             TEXT NOT NULL
        CHECK (scope IN ('whole_file', 'format_payload', 'decoded_mosaic')),
    algorithm         TEXT NOT NULL CHECK (length(algorithm) BETWEEN 1 AND 128),
    provider_id       TEXT NOT NULL DEFAULT '',
    provider_version  TEXT NOT NULL DEFAULT '',
    digest            BLOB NOT NULL CHECK (length(digest) = 32),
    source_byte_len   INTEGER NOT NULL CHECK (source_byte_len >= 0),
    source_modified_at_ms INTEGER,
    observed_at_ms    INTEGER NOT NULL,
    PRIMARY KEY (representation_id, scope, algorithm, provider_id, provider_version),
    UNIQUE (scope, algorithm, provider_id, provider_version, digest),
    FOREIGN KEY (representation_id) REFERENCES representations(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX representation_content_identity_representation_idx
    ON representation_content_identities(representation_id, observed_at_ms DESC);

CREATE TABLE photo_library_facts (
    photo_id                    BLOB PRIMARY KEY NOT NULL CHECK (length(photo_id) = 16),
    captured_at_unix_seconds    INTEGER,
    capture_day                 TEXT NOT NULL DEFAULT '',
    camera_make                 TEXT NOT NULL DEFAULT '',
    camera_model                TEXT NOT NULL DEFAULT '',
    camera_key                  TEXT NOT NULL DEFAULT '',
    lens_make                   TEXT NOT NULL DEFAULT '',
    lens_model                  TEXT NOT NULL DEFAULT '',
    lens_key                    TEXT NOT NULL DEFAULT '',
    aperture_milli              INTEGER,
    focal_length_tenth_mm       INTEGER,
    iso_speed                   REAL,
    latitude_e7                 INTEGER,
    longitude_e7                INTEGER,
    place_name                  TEXT NOT NULL DEFAULT '',
    indexed_representation_id   BLOB CHECK (indexed_representation_id IS NULL OR length(indexed_representation_id) = 16),
    indexed_source_byte_len     INTEGER,
    indexed_source_modified_at_ms INTEGER,
    indexed_at_ms               INTEGER NOT NULL,
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE CASCADE,
    FOREIGN KEY (indexed_representation_id) REFERENCES representations(id) ON DELETE SET NULL
) STRICT;

CREATE INDEX photo_library_facts_capture_idx
    ON photo_library_facts(captured_at_unix_seconds DESC, photo_id);
CREATE INDEX photo_library_facts_day_idx
    ON photo_library_facts(capture_day, captured_at_unix_seconds DESC, photo_id);
CREATE INDEX photo_library_facts_camera_idx
    ON photo_library_facts(camera_key, captured_at_unix_seconds DESC, photo_id);
CREATE INDEX photo_library_facts_lens_idx
    ON photo_library_facts(lens_key, captured_at_unix_seconds DESC, photo_id);
CREATE INDEX photo_library_facts_aperture_idx
    ON photo_library_facts(aperture_milli, captured_at_unix_seconds DESC, photo_id);

CREATE TABLE photo_library_state (
    photo_id       BLOB PRIMARY KEY NOT NULL CHECK (length(photo_id) = 16),
    liked          INTEGER NOT NULL DEFAULT 0 CHECK (liked IN (0, 1)),
    color_label    TEXT NOT NULL DEFAULT 'none',
    updated_at_ms  INTEGER NOT NULL,
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX photo_library_state_liked_idx ON photo_library_state(liked, photo_id);
CREATE INDEX photo_library_state_color_idx ON photo_library_state(color_label, photo_id);

CREATE TABLE library_albums (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    kind            TEXT NOT NULL CHECK (kind IN ('manual', 'smart')),
    name            TEXT NOT NULL CHECK (length(trim(name)) BETWEEN 1 AND 256),
    query_json      TEXT,
    created_at_ms   INTEGER NOT NULL,
    updated_at_ms   INTEGER NOT NULL,
    UNIQUE (name COLLATE NOCASE),
    CHECK ((kind = 'manual' AND query_json IS NULL) OR (kind = 'smart' AND json_valid(query_json)))
) STRICT;

CREATE INDEX library_albums_kind_name_idx ON library_albums(kind, name COLLATE NOCASE);

CREATE TABLE library_album_memberships (
    album_id        BLOB NOT NULL CHECK (length(album_id) = 16),
    photo_id        BLOB NOT NULL CHECK (length(photo_id) = 16),
    added_at_ms     INTEGER NOT NULL,
    sort_key        INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (album_id, photo_id),
    FOREIGN KEY (album_id) REFERENCES library_albums(id) ON DELETE CASCADE,
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX library_album_memberships_photo_idx
    ON library_album_memberships(photo_id, album_id);
CREATE INDEX library_album_memberships_page_idx
    ON library_album_memberships(album_id, sort_key, added_at_ms DESC, photo_id);
";

// The two covering traversal indexes below support the
// photo-first Library query. The query resolves one current original RAW
// representation and one current online location per photo, so these indexes
// avoid repeatedly sorting a photo's representation/location history at
// million-photo scale.
const SCHEMA_V1_LIBRARY_INDEXES: &str = r"
CREATE INDEX representations_photo_kind_current_idx
    ON representations(photo_id, kind, created_at_ms DESC, id DESC);
CREATE INDEX locations_representation_status_current_idx
    ON locations(representation_id, status, created_at_ms DESC, id DESC);
";

const SCHEMA_V1_STATE: &str = r"
CREATE TABLE catalog_schema (
    version       INTEGER PRIMARY KEY NOT NULL CHECK (version = 1),
    identity      TEXT NOT NULL CHECK (identity = 'shadow-catalog-v1-r26-identity-fingerprint-guard'),
    created_at_ms INTEGER NOT NULL
) STRICT;
";

// These are schema-v1 DDL fragments, ordered only by SQL foreign-key and
// `CREATE TABLE` dependencies. They are applied once to an empty catalog in a
// single transaction; they are not a migration history.
const SCHEMA_V1_COMPONENTS: &[&str] = &[
    SCHEMA_V1_CORE,
    SCHEMA_V1_IMPORT,
    SCHEMA_V1_DECODER,
    SCHEMA_V1_CACHE,
    SCHEMA_V1_RECIPE,
    SCHEMA_V1_FEEDBACK,
    SCHEMA_V1_TECHNICAL_OBSERVATION,
    SCHEMA_V1_DECISION,
    SCHEMA_V1_EDIT_REPOSITORY,
    export_queue::SCHEMA_V1_EXPORT_QUEUE,
    SCHEMA_V1_LIBRARY,
    SCHEMA_V1_LIBRARY_INDEXES,
];

#[derive(Debug, Error)]
pub enum CatalogError {
    #[error("SQLite catalog error: {0}")]
    Sqlite(#[from] rusqlite::Error),
    #[error(
        "development catalog reset required: found schema {found:?}; Shadow currently supports only a fresh catalog schema v1"
    )]
    DevelopmentCatalogResetRequired { found: Option<i64> },
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
    #[error(
        "cannot attach verified relocation because the target location is already registered: {display_path}"
    )]
    RelinkTargetLocationAlreadyRegistered { display_path: String },
    #[error(
        "cannot attach verified relocation because its exact identity is not recorded for expected representation {expected_representation_id}"
    )]
    RelinkIdentityNotRecorded {
        expected_representation_id: RepresentationId,
    },
    #[error(
        "cannot attach verified relocation because its exact identity belongs to representation {actual_representation_id}, not expected representation {expected_representation_id}"
    )]
    RelinkIdentityOwnerMismatch {
        expected_representation_id: RepresentationId,
        actual_representation_id: RepresentationId,
    },
    #[error(
        "representation {representation_id} changed while its content identity was being bound"
    )]
    ContentIdentitySourceChanged { representation_id: RepresentationId },
    #[error("cannot start catalog writer actor: {0}")]
    ActorStart(#[source] std::io::Error),
    #[error("catalog writer actor is unavailable")]
    ActorUnavailable,
    #[error("catalog writer actor panicked")]
    ActorPanicked,
    #[error("representation {0} does not exist")]
    RepresentationNotFound(RepresentationId),
    #[error("photo {0} does not exist")]
    PhotoNotFound(PhotoId),
    #[error("album {0} does not exist")]
    AlbumNotFound(shadow_domain::CollectionId),
    #[error("invalid content identity: {0}")]
    InvalidContentIdentity(String),
    #[error("invalid Library metadata facts: {0}")]
    InvalidLibraryFacts(String),
    #[error("invalid Library photo state: {0}")]
    InvalidLibraryState(String),
    #[error("invalid Library album: {0}")]
    InvalidAlbum(String),
    #[error("invalid Library query: {0}")]
    InvalidLibraryQuery(String),
    #[error("invalid export queue data: {0}")]
    InvalidExport(String),
    #[error("export preset {0} does not exist")]
    ExportPresetNotFound(export_queue::ExportPresetId),
    #[error("export preset revision {0} does not exist")]
    ExportPresetRevisionNotFound(export_queue::ExportPresetRevisionId),
    #[error("export job {0} does not exist")]
    ExportJobNotFound(export_queue::ExportJobId),
    #[error("export item {0} does not exist")]
    ExportItemNotFound(export_queue::ExportItemId),
    #[error(
        "export item {item_id} state did not match expected {expected:?}; actual state is {actual:?}"
    )]
    ExportItemStateMismatch {
        item_id: export_queue::ExportItemId,
        expected: export_queue::ExportItemState,
        actual: export_queue::ExportItemState,
    },
    #[error("export item {item_id} cannot transition from {from:?} to {to:?}")]
    InvalidExportTransition {
        item_id: export_queue::ExportItemId,
        from: export_queue::ExportItemState,
        to: export_queue::ExportItemState,
    },
    #[error("representation {representation_id} is not owned by export photo {photo_id}")]
    ExportRepresentationOwnerMismatch {
        photo_id: PhotoId,
        representation_id: RepresentationId,
    },
    #[error("export recipe snapshot digest does not match immutable commit {commit_id}")]
    ExportRecipeSnapshotMismatch {
        commit_id: shadow_domain::RecipeCommitId,
    },
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
    #[error("unknown persisted platform: {0}")]
    UnknownPlatform(String),
    #[error("invalid Recipe: {0}")]
    InvalidRecipe(String),
    #[error("Recipe JSON error: {0}")]
    RecipeJson(serde_json::Error),
    #[error("Recipe commit {0} does not exist")]
    RecipeCommitNotFound(shadow_domain::RecipeCommitId),
    #[error("Recipe commit {0} already exists and cannot be overwritten")]
    RecipeCommitAlreadyExists(shadow_domain::RecipeCommitId),
    #[error("Recipe commit {commit_id} is not owned by photo {photo_id}")]
    RecipeCommitOwnerMismatch {
        photo_id: PhotoId,
        commit_id: shadow_domain::RecipeCommitId,
    },
    #[error("invalid Recipe ref name: {0:?}")]
    InvalidRecipeRefName(String),
    #[error("Recipe ref name {0:?} appears more than once in one commit")]
    DuplicateRecipeRefName(String),
    #[error(
        "Recipe ref {name:?} for photo {photo_id} did not match expectation {expected:?}; current commit is {actual:?}"
    )]
    RecipeRefExpectationMismatch {
        photo_id: PhotoId,
        name: String,
        expected: RecipeRefExpectation,
        actual: Option<shadow_domain::RecipeCommitId>,
    },
    #[error("unknown persisted Recipe ref kind: {0}")]
    UnknownRecipeRefKind(String),
    #[error("invalid AI feedback: {0}")]
    InvalidFeedback(String),
    #[error("AI feedback event id {0:?} already exists and cannot be overwritten")]
    FeedbackEventAlreadyExists(String),
    #[error("AI feedback event id {0:?} does not exist")]
    FeedbackEventNotFound(String),
    #[error(
        "presented visual representation {representation_id} is not owned by candidate photo {photo_id}"
    )]
    FeedbackVisualRepresentationOwnerMismatch {
        photo_id: PhotoId,
        representation_id: RepresentationId,
    },
    #[error("AI feedback forget fact id {0:?} already exists and cannot be overwritten")]
    FeedbackForgetFactAlreadyExists(String),
    #[error("AI feedback page limit {limit} is outside 1 through {maximum}")]
    InvalidFeedbackPageLimit { limit: usize, maximum: usize },
    #[error("AI feedback sequence space is exhausted")]
    FeedbackSequenceExhausted,
    #[error("AI feedback JSON error: {0}")]
    FeedbackJson(serde_json::Error),
    #[error("persisted AI feedback failed its integrity check: {0}")]
    InvalidPersistedFeedback(&'static str),
    #[error("invalid photo decision: {0}")]
    InvalidPhotoDecision(String),
    #[error("photo decision event id {0:?} already exists and cannot be overwritten")]
    PhotoDecisionEventAlreadyExists(String),
    #[error(
        "photo decision head for {photo_id} did not match expected sequence {expected}; current sequence is {actual}"
    )]
    PhotoDecisionHeadMismatch {
        photo_id: PhotoId,
        expected: u64,
        actual: u64,
    },
    #[error(
        "photo decision before-state for {photo_id} disagrees with head {head_sequence}: expected {expected_flag:?}/{expected_rating}, current {actual_flag:?}/{actual_rating}"
    )]
    PhotoDecisionBeforeStateMismatch {
        photo_id: PhotoId,
        head_sequence: u64,
        expected_flag: PhotoFlag,
        expected_rating: u8,
        actual_flag: PhotoFlag,
        actual_rating: u8,
    },
    #[error("photo decision history page limit {limit} is outside 1 through {maximum}")]
    InvalidPhotoDecisionPageLimit { limit: usize, maximum: usize },
    #[error("photo decision sequence space is exhausted")]
    PhotoDecisionSequenceExhausted,
    #[error("photo decision JSON error: {0}")]
    PhotoDecisionJson(serde_json::Error),
    #[error("persisted photo decision failed its integrity check: {0}")]
    InvalidPersistedPhotoDecision(&'static str),
    #[error("invalid technical observation: {0}")]
    InvalidTechnicalObservation(String),
    #[error("technical observation JSON error: {0}")]
    TechnicalObservationJson(serde_json::Error),
    #[error("technical observation field {field} is outside SQLite's integer range")]
    TechnicalObservationValueOutOfRange { field: &'static str },
    #[error("persisted technical observation failed its integrity check: {0}")]
    InvalidPersistedTechnicalObservation(&'static str),
    #[error("invalid edit repository object: {0}")]
    InvalidEditObject(String),
    #[error("edit object {0} does not exist")]
    EditObjectNotFound(shadow_domain::EditObjectId),
    #[error("content-addressed edit object {0} conflicts with persisted bytes or edges")]
    EditObjectCollision(shadow_domain::EditObjectId),
    #[error("invalid edit repository commit: {0}")]
    InvalidEditRepositoryCommit(String),
    #[error("edit repository commit {0} does not exist")]
    EditRepositoryCommitNotFound(shadow_domain::EditCommitId),
    #[error("content-addressed edit repository commit {0} conflicts with persisted bytes")]
    EditRepositoryCommitCollision(shadow_domain::EditCommitId),
    #[error("invalid edit repository ref name: {0:?}")]
    InvalidEditRepositoryRefName(String),
    #[error("edit repository ref name {0:?} appears more than once in one commit")]
    DuplicateEditRepositoryRefName(String),
    #[error(
        "edit repository ref {name:?} did not match expectation {expected:?}; current commit is {actual:?}"
    )]
    EditRepositoryRefExpectationMismatch {
        name: String,
        expected: shadow_domain::EditRepositoryRefExpectation,
        actual: Option<shadow_domain::EditCommitId>,
    },
    #[error("unknown persisted edit repository ref kind: {0}")]
    UnknownEditRepositoryRefKind(String),
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
    /// Opens or creates a file-backed catalog using the one current development
    /// schema. A catalog from an earlier development shape is rejected rather
    /// than migrated.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` cannot open, configure, or initialize
    /// the catalog.
    pub fn open(path: &Path) -> Result<Self, CatalogError> {
        let mut connection = Connection::open(path)?;
        configure_connection(&connection, true)?;
        initialize_schema_v1(&mut connection)?;
        Ok(Self { connection })
    }

    /// Opens an isolated in-memory catalog, primarily for tests.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` cannot initialize the
    /// in-memory database.
    pub fn open_in_memory() -> Result<Self, CatalogError> {
        let mut connection = Connection::open_in_memory()?;
        configure_connection(&connection, false)?;
        initialize_schema_v1(&mut connection)?;
        Ok(Self { connection })
    }

    /// Returns the active catalog schema version.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the schema state cannot be queried.
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
            let byte_len = i64::try_from(request.byte_len)
                .map_err(|error| rusqlite::Error::ToSqlConversionFailure(Box::new(error)))?;
            // A location may be overwritten in place. Its representation
            // keeps the logical photo identity, but every byte-derived proof
            // belongs to the old revision and must be discarded atomically.
            // Updating the representation fingerprint also naturally
            // invalidates decoder snapshots and cache artifacts keyed by it.
            transaction.execute(
                "UPDATE representations SET byte_len = ?2, modified_at_ms = ?3 WHERE id = ?1",
                params![
                    existing.representation_id.as_bytes().as_slice(),
                    byte_len,
                    request.modified_at_ms,
                ],
            )?;
            transaction.execute(
                "DELETE FROM representation_content_identities WHERE representation_id = ?1",
                [existing.representation_id.as_bytes().as_slice()],
            )?;
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

fn initialize_schema_v1(connection: &mut Connection) -> Result<(), CatalogError> {
    if table_exists(connection, "catalog_schema")? {
        let identity: Option<String> = connection
            .query_row(
                "SELECT identity FROM catalog_schema WHERE version = ?1",
                [SCHEMA_VERSION],
                |row| row.get(0),
            )
            .optional()?;
        if identity.as_deref() == Some("shadow-catalog-v1-r26-identity-fingerprint-guard")
            && current_schema_version(connection)? == SCHEMA_VERSION
        {
            return Ok(());
        }
        return Err(CatalogError::DevelopmentCatalogResetRequired {
            found: current_schema_version(connection).ok(),
        });
    }

    if table_exists(connection, "schema_migrations")? {
        return Err(CatalogError::DevelopmentCatalogResetRequired {
            found: legacy_schema_version(connection).ok(),
        });
    }

    if catalog_tables_exist(connection)? {
        return Err(CatalogError::DevelopmentCatalogResetRequired { found: None });
    }

    let transaction = connection.transaction()?;
    transaction.execute_batch(SCHEMA_V1_STATE)?;
    for component in SCHEMA_V1_COMPONENTS {
        transaction.execute_batch(component)?;
    }
    transaction.execute(
        "INSERT INTO catalog_schema(version, identity, created_at_ms)
         VALUES (?1, 'shadow-catalog-v1-r26-identity-fingerprint-guard', unixepoch('subsec') * 1000)",
        [SCHEMA_VERSION],
    )?;
    transaction.commit()?;
    Ok(())
}

fn table_exists(connection: &Connection, table: &str) -> rusqlite::Result<bool> {
    connection
        .query_row(
            "SELECT EXISTS(
             SELECT 1 FROM sqlite_schema WHERE type = 'table' AND name = ?1
         )",
            [table],
            |row| row.get::<_, i64>(0),
        )
        .map(|exists| exists != 0)
}

fn catalog_tables_exist(connection: &Connection) -> rusqlite::Result<bool> {
    connection
        .query_row(
            "SELECT EXISTS(
             SELECT 1 FROM sqlite_schema
             WHERE type = 'table' AND name NOT LIKE 'sqlite_%'
         )",
            [],
            |row| row.get::<_, i64>(0),
        )
        .map(|exists| exists != 0)
}

fn current_schema_version(connection: &Connection) -> rusqlite::Result<i64> {
    connection.query_row(
        "SELECT version FROM catalog_schema
         WHERE identity = 'shadow-catalog-v1-r26-identity-fingerprint-guard'",
        [],
        |row| row.get(0),
    )
}

fn legacy_schema_version(connection: &Connection) -> rusqlite::Result<i64> {
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
    fn schema_v1_creates_current_catalog_shape() {
        let catalog = Catalog::open_in_memory().expect("open catalog");

        assert_eq!(catalog.schema_version().expect("schema version"), 1);
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
    }

    #[test]
    fn prior_v1_identity_is_rejected_for_a_development_reset() {
        let mut connection = Connection::open_in_memory().expect("open prior v1 fixture");
        configure_connection(&connection, false).expect("configure prior v1 fixture");
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
            initialize_schema_v1(&mut connection),
            Err(CatalogError::DevelopmentCatalogResetRequired { found: None })
        ));
        assert!(table_exists(&connection, "catalog_schema").expect("preserve reset marker"));
    }

    #[test]
    fn legacy_catalog_is_rejected_without_a_migration_attempt() {
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
            initialize_schema_v1(&mut connection),
            Err(CatalogError::DevelopmentCatalogResetRequired { found: Some(14) })
        ));
        assert!(table_exists(&connection, "schema_migrations").expect("legacy marker remains"));
        assert!(!table_exists(&connection, "catalog_schema").expect("no partial v1 state"));
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
