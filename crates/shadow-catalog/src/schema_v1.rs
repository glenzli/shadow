//! Authoritative construction and identity checks for the single supported
//! development Catalog schema.
//!
//! Shadow has no migration chain before its first compatibility promise. This
//! module creates schema v1 atomically and rejects every other persisted shape.

use rusqlite::{Connection, OptionalExtension};

use crate::{CatalogError, export_queue};

/// The only on-disk Catalog shape supported by this development build.
pub(crate) const SCHEMA_VERSION: i64 = 1;

const SCHEMA_IDENTITY: &str = "shadow-catalog-v1-r29-library-keywords";

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

CREATE TABLE photo_library_metadata_overrides (
    photo_id                    BLOB PRIMARY KEY NOT NULL CHECK (length(photo_id) = 16),
    capture_time_mode           TEXT CHECK (capture_time_mode IN ('set', 'clear')),
    captured_at_unix_seconds    INTEGER,
    capture_day                 TEXT NOT NULL DEFAULT '',
    capture_time_origin         TEXT NOT NULL DEFAULT '',
    capture_time_source_label   TEXT NOT NULL DEFAULT '',
    capture_time_updated_at_ms  INTEGER,
    coordinates_mode            TEXT CHECK (coordinates_mode IN ('set', 'clear')),
    latitude_e7                 INTEGER,
    longitude_e7                INTEGER,
    place_name                  TEXT NOT NULL DEFAULT '',
    coordinates_origin          TEXT NOT NULL DEFAULT '',
    coordinates_source_label    TEXT NOT NULL DEFAULT '',
    coordinates_updated_at_ms   INTEGER,
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE CASCADE,
    CHECK (
        (capture_time_mode IS NULL
         AND captured_at_unix_seconds IS NULL
         AND capture_day = ''
         AND capture_time_origin = ''
         AND capture_time_source_label = ''
         AND capture_time_updated_at_ms IS NULL)
        OR
        (capture_time_mode = 'clear'
         AND captured_at_unix_seconds IS NULL
         AND capture_day = ''
         AND capture_time_origin IN ('manual', 'gpx')
         AND capture_time_updated_at_ms >= 0)
        OR
        (capture_time_mode = 'set'
         AND captured_at_unix_seconds IS NOT NULL
         AND length(capture_day) = 10
         AND capture_time_origin IN ('manual', 'gpx')
         AND capture_time_updated_at_ms >= 0)
    ),
    CHECK (
        (coordinates_mode IS NULL
         AND latitude_e7 IS NULL
         AND longitude_e7 IS NULL
         AND place_name = ''
         AND coordinates_origin = ''
         AND coordinates_source_label = ''
         AND coordinates_updated_at_ms IS NULL)
        OR
        (coordinates_mode = 'clear'
         AND latitude_e7 IS NULL
         AND longitude_e7 IS NULL
         AND place_name = ''
         AND coordinates_origin IN ('manual', 'gpx')
         AND coordinates_updated_at_ms >= 0)
        OR
        (coordinates_mode = 'set'
         AND latitude_e7 BETWEEN -900000000 AND 900000000
         AND longitude_e7 BETWEEN -1800000000 AND 1800000000
         AND coordinates_origin IN ('manual', 'gpx')
         AND coordinates_updated_at_ms >= 0)
    ),
    CHECK (length(capture_time_source_label) <= 1024),
    CHECK (length(coordinates_source_label) <= 1024),
    CHECK (length(place_name) <= 1024)
) STRICT;

CREATE TABLE photo_library_effective_facts (
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

CREATE INDEX photo_library_effective_capture_idx
    ON photo_library_effective_facts(captured_at_unix_seconds DESC, photo_id);
CREATE INDEX photo_library_effective_day_idx
    ON photo_library_effective_facts(capture_day, captured_at_unix_seconds DESC, photo_id);
CREATE INDEX photo_library_effective_camera_idx
    ON photo_library_effective_facts(camera_key, captured_at_unix_seconds DESC, photo_id);
CREATE INDEX photo_library_effective_lens_idx
    ON photo_library_effective_facts(lens_key, captured_at_unix_seconds DESC, photo_id);
CREATE INDEX photo_library_effective_aperture_idx
    ON photo_library_effective_facts(aperture_milli, captured_at_unix_seconds DESC, photo_id);
CREATE INDEX photo_library_effective_geo_idx
    ON photo_library_effective_facts(latitude_e7, longitude_e7, photo_id)
    WHERE latitude_e7 IS NOT NULL AND longitude_e7 IS NOT NULL;

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

CREATE TABLE library_keywords (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    parent_id       BLOB CHECK (parent_id IS NULL OR length(parent_id) = 16),
    name            TEXT NOT NULL CHECK (length(trim(name)) BETWEEN 1 AND 256),
    normalized_name TEXT NOT NULL CHECK (length(normalized_name) BETWEEN 1 AND 256),
    created_at_ms   INTEGER NOT NULL,
    updated_at_ms   INTEGER NOT NULL,
    CHECK (parent_id IS NULL OR parent_id <> id),
    FOREIGN KEY (parent_id) REFERENCES library_keywords(id) ON DELETE CASCADE
) STRICT;

CREATE UNIQUE INDEX library_keywords_root_name_idx
    ON library_keywords(normalized_name) WHERE parent_id IS NULL;
CREATE UNIQUE INDEX library_keywords_child_name_idx
    ON library_keywords(parent_id, normalized_name) WHERE parent_id IS NOT NULL;
CREATE INDEX library_keywords_parent_name_idx
    ON library_keywords(parent_id, normalized_name, id);

CREATE TABLE library_photo_keywords (
    keyword_id       BLOB NOT NULL CHECK (length(keyword_id) = 16),
    photo_id         BLOB NOT NULL CHECK (length(photo_id) = 16),
    origin           TEXT NOT NULL CHECK (origin IN ('manual', 'imported', 'ai_accepted')),
    source_label     TEXT NOT NULL DEFAULT '' CHECK (length(source_label) <= 512),
    confidence_milli INTEGER CHECK (confidence_milli BETWEEN 0 AND 1000),
    assigned_at_ms   INTEGER NOT NULL,
    PRIMARY KEY (keyword_id, photo_id),
    FOREIGN KEY (keyword_id) REFERENCES library_keywords(id) ON DELETE CASCADE,
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE CASCADE
) STRICT;

CREATE INDEX library_photo_keywords_photo_idx
    ON library_photo_keywords(photo_id, keyword_id);
CREATE INDEX library_photo_keywords_keyword_idx
    ON library_photo_keywords(keyword_id, photo_id);
";

// Covering traversal indexes support the photo-first Library query, which
// resolves one current original RAW representation and online location per
// photo without repeatedly sorting their history.
const SCHEMA_V1_LIBRARY_INDEXES: &str = r"
CREATE INDEX representations_photo_kind_current_idx
    ON representations(photo_id, kind, created_at_ms DESC, id DESC);
CREATE INDEX locations_representation_status_current_idx
    ON locations(representation_id, status, created_at_ms DESC, id DESC);
";

const SCHEMA_V1_STATE: &str = r"
CREATE TABLE catalog_schema (
    version       INTEGER PRIMARY KEY NOT NULL CHECK (version = 1),
    identity      TEXT NOT NULL CHECK (identity = 'shadow-catalog-v1-r29-library-keywords'),
    created_at_ms INTEGER NOT NULL
) STRICT;
";

// Ordered only by SQL foreign-key and CREATE TABLE dependencies. These
// fragments are applied once to an empty Catalog in one transaction; they are
// not a migration history.
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

pub(crate) fn initialize(connection: &mut Connection) -> Result<(), CatalogError> {
    if table_exists(connection, "catalog_schema")? {
        let identity: Option<String> = connection
            .query_row(
                "SELECT identity FROM catalog_schema WHERE version = ?1",
                [SCHEMA_VERSION],
                |row| row.get(0),
            )
            .optional()?;
        if identity.as_deref() == Some(SCHEMA_IDENTITY)
            && current_version(connection)? == SCHEMA_VERSION
        {
            return Ok(());
        }
        return Err(CatalogError::DevelopmentCatalogResetRequired {
            found: current_version(connection).ok(),
        });
    }

    if table_exists(connection, "schema_migrations")? {
        return Err(CatalogError::DevelopmentCatalogResetRequired {
            found: legacy_version(connection).ok(),
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
         VALUES (?1, ?2, unixepoch('subsec') * 1000)",
        (SCHEMA_VERSION, SCHEMA_IDENTITY),
    )?;
    transaction.commit()?;
    Ok(())
}

pub(crate) fn table_exists(connection: &Connection, table: &str) -> rusqlite::Result<bool> {
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

pub(crate) fn catalog_tables_exist(connection: &Connection) -> rusqlite::Result<bool> {
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

pub(crate) fn current_version(connection: &Connection) -> rusqlite::Result<i64> {
    connection.query_row(
        "SELECT version FROM catalog_schema WHERE identity = ?1",
        [SCHEMA_IDENTITY],
        |row| row.get(0),
    )
}

fn legacy_version(connection: &Connection) -> rusqlite::Result<i64> {
    connection.query_row(
        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations",
        [],
        |row| row.get(0),
    )
}

#[cfg(test)]
mod tests;
