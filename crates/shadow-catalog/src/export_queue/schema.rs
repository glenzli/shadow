//! Exact schema-v1 SQL owned by durable export persistence.

/// Schema-v1 component for durable export work. The numeric catalog schema is
/// deliberately still v1 while development is unstable; changing this shape
/// changes the v1 identity marker and requires a development-catalog reset.
pub(crate) const SCHEMA_V1_EXPORT_QUEUE: &str = r"
CREATE TABLE export_presets (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    name            TEXT NOT NULL CHECK (length(trim(name)) BETWEEN 1 AND 256),
    created_at_ms   INTEGER NOT NULL,
    archived_at_ms  INTEGER,
    UNIQUE (name COLLATE NOCASE)
) STRICT;

CREATE TABLE export_preset_revisions (
    id              BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    preset_id       BLOB NOT NULL CHECK (length(preset_id) = 16),
    revision        INTEGER NOT NULL CHECK (revision > 0),
    settings_json   TEXT NOT NULL CHECK (json_valid(settings_json)),
    settings_digest BLOB NOT NULL CHECK (length(settings_digest) = 32),
    created_at_ms   INTEGER NOT NULL,
    UNIQUE (preset_id, revision),
    FOREIGN KEY (preset_id) REFERENCES export_presets(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX export_preset_revisions_preset_time_idx
    ON export_preset_revisions(preset_id, revision DESC);

CREATE TRIGGER export_preset_revisions_no_update
BEFORE UPDATE ON export_preset_revisions
BEGIN
    SELECT RAISE(ABORT, 'export preset revisions are immutable');
END;

CREATE TRIGGER export_preset_revisions_no_delete
BEFORE DELETE ON export_preset_revisions
BEGIN
    SELECT RAISE(ABORT, 'export preset revisions are immutable');
END;

CREATE TABLE export_jobs (
    id                  BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    state               TEXT NOT NULL
        CHECK (state IN ('queued', 'running', 'paused_conflict', 'cancelled',
                         'failed', 'completed', 'interrupted')),
    preset_revision_id  BLOB CHECK (preset_revision_id IS NULL OR length(preset_revision_id) = 16),
    settings_json       TEXT NOT NULL CHECK (json_valid(settings_json)),
    settings_digest     BLOB NOT NULL CHECK (length(settings_digest) = 32),
    created_at_ms       INTEGER NOT NULL,
    updated_at_ms       INTEGER NOT NULL,
    started_at_ms       INTEGER,
    finished_at_ms      INTEGER,
    last_error_code     TEXT,
    last_error_message  TEXT,
    last_error_retryable INTEGER CHECK (last_error_retryable IN (0, 1)),
    CHECK (
        (last_error_code IS NULL AND last_error_message IS NULL AND last_error_retryable IS NULL)
        OR
        (length(trim(last_error_code)) BETWEEN 1 AND 128
         AND length(trim(last_error_message)) BETWEEN 1 AND 8192
         AND last_error_retryable IN (0, 1))
    ),
    FOREIGN KEY (preset_revision_id) REFERENCES export_preset_revisions(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX export_jobs_state_updated_idx
    ON export_jobs(state, updated_at_ms DESC, id);

CREATE TRIGGER export_jobs_snapshot_immutable
BEFORE UPDATE OF preset_revision_id, settings_json, settings_digest ON export_jobs
BEGIN
    SELECT RAISE(ABORT, 'export job snapshot is immutable');
END;

CREATE TABLE export_items (
    id                      BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    job_id                  BLOB NOT NULL CHECK (length(job_id) = 16),
    position                INTEGER NOT NULL CHECK (position >= 0),
    photo_id                BLOB NOT NULL CHECK (length(photo_id) = 16),
    representation_id       BLOB NOT NULL CHECK (length(representation_id) = 16),
    recipe_commit_id        BLOB NOT NULL CHECK (length(recipe_commit_id) = 16),
    recipe_snapshot_digest  BLOB NOT NULL CHECK (length(recipe_snapshot_digest) = 32),
    edit_commit_id          BLOB CHECK (edit_commit_id IS NULL OR length(edit_commit_id) = 32),
    source_identity_json    TEXT NOT NULL CHECK (json_valid(source_identity_json)),
    source_identity_digest  BLOB NOT NULL CHECK (length(source_identity_digest) = 32),
    render_plan_json        TEXT NOT NULL CHECK (json_valid(render_plan_json)),
    render_plan_digest      BLOB NOT NULL CHECK (length(render_plan_digest) = 32),
    output_platform         TEXT NOT NULL,
    output_native_path      BLOB NOT NULL CHECK (length(output_native_path) > 0),
    output_display_path     TEXT NOT NULL CHECK (length(trim(output_display_path)) > 0),
    state                   TEXT NOT NULL
        CHECK (state IN ('queued', 'preparing', 'rendering', 'encoding', 'writing_temp',
                         'paused_conflict', 'cancelled', 'failed', 'completed', 'interrupted')),
    attempt_count           INTEGER NOT NULL DEFAULT 0 CHECK (attempt_count >= 0),
    created_at_ms           INTEGER NOT NULL,
    updated_at_ms           INTEGER NOT NULL,
    started_at_ms           INTEGER,
    finished_at_ms          INTEGER,
    error_code              TEXT,
    error_message           TEXT,
    error_retryable         INTEGER CHECK (error_retryable IN (0, 1)),
    CHECK (
        (error_code IS NULL AND error_message IS NULL AND error_retryable IS NULL)
        OR
        (length(trim(error_code)) BETWEEN 1 AND 128
         AND length(trim(error_message)) BETWEEN 1 AND 8192
         AND error_retryable IN (0, 1))
    ),
    UNIQUE (job_id, position),
    FOREIGN KEY (job_id) REFERENCES export_jobs(id) ON DELETE CASCADE,
    FOREIGN KEY (photo_id) REFERENCES photos(id) ON DELETE RESTRICT,
    FOREIGN KEY (representation_id) REFERENCES representations(id) ON DELETE RESTRICT,
    FOREIGN KEY (recipe_commit_id, photo_id)
        REFERENCES recipe_commits(id, photo_id) ON DELETE RESTRICT,
    FOREIGN KEY (edit_commit_id) REFERENCES edit_repository_commits(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX export_items_worker_claim_idx
    ON export_items(state, created_at_ms, job_id, position);
CREATE INDEX export_items_job_position_idx
    ON export_items(job_id, position);
CREATE INDEX export_items_photo_idx
    ON export_items(photo_id, created_at_ms DESC);

CREATE TRIGGER export_items_snapshot_immutable
BEFORE UPDATE OF job_id, position, photo_id, representation_id, recipe_commit_id,
                 recipe_snapshot_digest, edit_commit_id, source_identity_json,
                 source_identity_digest, render_plan_json, render_plan_digest,
                 output_platform, output_native_path, output_display_path
ON export_items
BEGIN
    SELECT RAISE(ABORT, 'export item snapshot is immutable');
END;

CREATE TABLE export_output_receipts (
    id                  BLOB PRIMARY KEY NOT NULL CHECK (length(id) = 16),
    item_id             BLOB NOT NULL UNIQUE CHECK (length(item_id) = 16),
    output_platform     TEXT NOT NULL,
    output_native_path  BLOB NOT NULL CHECK (length(output_native_path) > 0),
    output_display_path TEXT NOT NULL CHECK (length(trim(output_display_path)) > 0),
    output_format       TEXT NOT NULL CHECK (length(trim(output_format)) BETWEEN 1 AND 64),
    byte_len            INTEGER NOT NULL CHECK (byte_len >= 0),
    content_digest      BLOB CHECK (content_digest IS NULL OR length(content_digest) = 32),
    receipt_json        TEXT NOT NULL CHECK (json_valid(receipt_json)),
    created_at_ms       INTEGER NOT NULL,
    FOREIGN KEY (item_id) REFERENCES export_items(id) ON DELETE RESTRICT
) STRICT;

CREATE INDEX export_output_receipts_created_idx
    ON export_output_receipts(created_at_ms DESC, id);

CREATE TRIGGER export_output_receipts_no_update
BEFORE UPDATE ON export_output_receipts
BEGIN
    SELECT RAISE(ABORT, 'export output receipts are immutable');
END;

CREATE TRIGGER export_output_receipts_no_delete
BEFORE DELETE ON export_output_receipts
BEGIN
    SELECT RAISE(ABORT, 'export output receipts are immutable');
END;
";
