//! Persistent, Qt-free application entry for Shadow's headless project CLI.
//!
//! This adapter owns the process protocol and managed original admission. It
//! delegates edit validation/publication, history and full-quality rendering to
//! the same application services as `DesktopSession`. No Recipe is transplanted
//! through the lossy cross-photo interchange projection.

mod edit;
mod export;
mod protocol;
mod review;

use std::{
    fs,
    io::{Read, Write},
    path::Path,
};

use anyhow::{Context, Result};
use serde_json::{Value, json};
use shadow_catalog::CatalogError;
use shadow_domain::PhotoId;

use crate::{DesktopSession, ffi, native_path_ffi, open_desktop_session_at};
use protocol::{Command, Request, SnapshotGuard};
pub use protocol::{ErrorResponse, MAX_REQUEST_BYTES, Response, SCHEMA_VERSION};

const MAX_SOURCE_BYTES: u64 = 2 * 1024 * 1024 * 1024;

#[derive(Debug, thiserror::Error)]
#[error("{message}")]
struct Fault {
    code: &'static str,
    message: String,
}

fn fault(code: &'static str, message: impl Into<String>) -> anyhow::Error {
    Fault {
        code,
        message: message.into(),
    }
    .into()
}

/// Execute one bounded request. Callers must use a CLI-owned, offline project;
/// live unsaved desktop drafts belong to the desktop session-owner protocol.
/// This function never starts an Infer request or writes an input photograph.
pub fn execute_json(project_root: &Path, request_bytes: &[u8]) -> Response {
    if request_bytes.len() > MAX_REQUEST_BYTES {
        return Response::failure("request_too_large", "request exceeds 65536 bytes");
    }
    let request: Request = match serde_json::from_slice(request_bytes) {
        Ok(value) => value,
        Err(error) => return Response::failure("invalid_request", error.to_string()),
    };
    if request.schema_version != SCHEMA_VERSION {
        return Response::failure("unsupported_schema", "supported schema_version is 1");
    }
    match execute(project_root, request.command) {
        Ok(result) => Response::success(result),
        Err(error) => error_response(&error),
    }
}

fn error_response(error: &anyhow::Error) -> Response {
    if let Some(fault) = error.downcast_ref::<Fault>() {
        return Response::failure(fault.code, &fault.message);
    }
    let code = match error.downcast_ref::<CatalogError>() {
        Some(
            CatalogError::RecipeRefExpectationMismatch { .. }
            | CatalogError::PhotoVariantExpectationMismatch { .. }
            | CatalogError::EditRepositoryRefExpectationMismatch { .. }
            | CatalogError::PhotoDecisionHeadMismatch { .. }
            | CatalogError::PhotoDecisionBeforeStateMismatch { .. },
        ) => "conflict",
        Some(CatalogError::InvalidPhotoDecision(_)) => "invalid_decision",
        _ if error.downcast_ref::<std::io::Error>().is_some() => "io_error",
        _ => "operation_failed",
    };
    Response::failure(code, format!("{error:#}"))
}

fn execute(root: &Path, command: Command) -> Result<Value> {
    if matches!(command, Command::Capabilities {}) {
        return Ok(capabilities());
    }
    let session = open_project(root, matches!(command, Command::Init {}))?;
    match command {
        Command::Capabilities {} => unreachable!("handled before opening a project"),
        Command::Init {} => Ok(
            json!({"catalog": shadow_core::native_location(&root.join("catalog.sqlite")), "cache": shadow_core::native_location(&root.join("cache"))}),
        ),
        Command::Import { source } => import(&session, root, &source),
        Command::Photos {
            filter,
            after,
            limit,
        } => photo_page(&session, &filter, after, limit),
        Command::Snapshot {
            photo_id,
            commit_id,
        } => snapshot(&session, root, &photo_id, commit_id.as_deref()),
        Command::Commit { guard, edits } => {
            let (source, mut settings) = load_guard(&session, root, &guard)?;
            edit::apply(&mut settings, &edits)?;
            // All validation precedes this existing SQLite CAS transaction.
            // No mutable postcommit readback can misidentify the acknowledgement.
            let receipt = session.commit_edit_tool_draft(
                &guard.photo_id,
                &source,
                &guard.representation_id,
                &guard.base_commit_id,
                &guard.expected_working_commit_id,
                &guard.expected_active_variant_id,
                &settings,
            )?;
            Ok(json!({
                "photo_id": receipt.photo_id, "representation_id": receipt.representation_id,
                "variant_id": receipt.variant_id, "commit_id": receipt.commit_id,
                "recipe_id": receipt.recipe_id, "snapshot_digest": receipt.snapshot_digest,
                "editable": edit::editable_projection(&receipt.settings)
            }))
        }
        Command::History {
            photo_id,
            after,
            limit,
        } => history_page(&session, &photo_id, after, limit),
        Command::Decision { photo_id } => review::state(&session, &photo_id),
        Command::SetDecision {
            photo_id,
            expected_head_sequence,
            flag,
            rating,
            provenance,
        } => review::set(
            &session,
            &photo_id,
            expected_head_sequence,
            flag,
            rating,
            &provenance,
        ),
        Command::DecisionHistory {
            photo_id,
            after,
            limit,
        } => review::history(&session, &photo_id, after, limit),
        Command::Export {
            guard,
            destination,
            bit_depth,
        } => {
            let (source, settings) = load_guard(&session, root, &guard)?;
            export::run(
                &session,
                root,
                &guard,
                &source,
                settings,
                &destination,
                bit_depth,
            )
        }
    }
}

fn open_project(root: &Path, initialize: bool) -> Result<Box<DesktopSession>> {
    let catalog = root.join("catalog.sqlite");
    if initialize {
        fs::create_dir_all(root).context("create project directory")?;
        fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&catalog)
            .map_err(|error| {
                if error.kind() == std::io::ErrorKind::AlreadyExists {
                    fault(
                        "already_exists",
                        "project Catalog already exists; open it with snapshot/history instead",
                    )
                } else {
                    error.into()
                }
            })?;
    } else if !catalog.is_file() {
        return Err(fault(
            "project_not_initialized",
            "initialize this project before opening it",
        ));
    }
    open_desktop_session_at(&catalog, &root.join("cache"))
}

fn photo_page(
    session: &DesktopSession,
    filter: &shadow_catalog::LibraryPhotoFilter,
    after: Option<protocol::PhotoCursor>,
    limit: u32,
) -> Result<Value> {
    if !(1..=100).contains(&limit) {
        return Err(fault(
            "invalid_request",
            "photo page limit must be in 1..=100",
        ));
    }
    let after = after
        .map(|cursor| -> Result<_> {
            Ok(shadow_catalog::LibraryPhotoCursor {
                photo_id: cursor.photo_id.parse()?,
                value: shadow_catalog::LibraryPhotoCursorValue::FileName(cursor.file_name),
            })
        })
        .transpose()?;
    let page = session.catalog.library_photo_page(
        filter,
        shadow_catalog::LibraryPhotoOrder::FileNameAscending,
        after.as_ref(),
        limit as usize,
    )?;
    let cursor = page.next_cursor.map(|cursor| match cursor.value {
        shadow_catalog::LibraryPhotoCursorValue::FileName(file_name) => json!({
            "photo_id": cursor.photo_id.to_string(), "file_name": file_name,
        }),
        shadow_catalog::LibraryPhotoCursorValue::CaptureTime(_) => {
            unreachable!("file-name query returns a matching cursor")
        }
    });
    Ok(json!({"items": page.items.into_iter().map(|item| json!({
                "photo_id": item.photo_id.to_string(), "representation_id": item.representation_id.to_string(),
                "source_location": item.location, "source_bytes": item.source.byte_len,
                "has_development_edits": item.has_development_edits
            })).collect::<Vec<_>>(), "has_more": cursor.is_some(), "next_cursor": cursor}))
}

fn history_page(
    session: &DesktopSession,
    photo_id: &str,
    after: Option<protocol::HistoryCursor>,
    limit: u32,
) -> Result<Value> {
    if !(1..=100).contains(&limit) {
        return Err(fault("invalid_request", "history limit must be in 1..=100"));
    }
    let cursor = after.map_or_else(ffi::FfiHistoryCursor::default, |after| {
        ffi::FfiHistoryCursor {
            created_at_ms: after.created_at_ms,
            commit_id: after.commit_id,
        }
    });
    let page = session.photo_edit_history_page(photo_id, &cursor, limit)?;
    Ok(json!({
        "entries": page.entries.into_iter().map(|entry| json!({
            "commit_id": entry.commit_id, "name": entry.name,
            "created_at_ms": entry.created_at_ms, "parent_commit_ids": entry.parent_commit_ids,
            "is_named": entry.is_named, "is_working": entry.is_working,
            "changed_parameter_keys": entry.changed_parameter_keys,
            "has_other_changes": entry.has_other_changes
        })).collect::<Vec<_>>(),
        "has_more": page.has_more,
        "next_cursor": if page.has_more { json!({
            "created_at_ms": page.next_cursor.created_at_ms, "commit_id": page.next_cursor.commit_id
        }) } else { Value::Null }
    }))
}

fn capabilities() -> Value {
    json!({
        "application": "shadow-cli", "phase": "persistent_project_v1",
        "commands": ["capabilities", "init", "import", "photos", "snapshot", "commit", "history", "export",
            "decision", "set_decision", "decision_history"],
        "review_decisions": {
            "authority": "user_directed", "executors": ["user", "agent"],
            "flags": ["unflagged", "picked", "rejected"],
            "rating_min": 0, "rating_max": shadow_domain::MAX_PHOTO_RATING,
            "concurrency": "expected_head_sequence CAS",
            "provenance": "required caller assertion; not authentication",
            "request_provenance_storage": "response_only; ledger records human origin, not executor",
            "autonomous_scoring": "unsupported", "no_op": "invalid_decision",
            "history_order": "ascending global sequence, filtered to one photo",
            "max_history_page": 100
        },
        "edits": ["set_basic", "set_node", "add_node", "remove_node", "move_node", "set_geometry"],
        "originals": "managed_byte_exact_copy_read_only_by_shadow",
        "input": {"raster_extensions": shadow_bridge::photo_supported_raster_extensions(),
            "raw": "public_decoder_dependent; proprietary providers are not bundled",
            "max_source_bytes": MAX_SOURCE_BYTES,
            "png": "RGB8 or fully opaque RGBA8; untagged/sRGB; bounded provider; not general PNG"},
        "output": {"format": "png", "bit_depths": [8, 16], "color_space": "display_srgb",
            "resizing": "unsupported", "overwrite": false,
            "metadata": "sRGB chunk only; no source EXIF/GPS/ICC copying"},
        "cancel": "unsupported",
        "ai": {"generation": "unsupported", "infer_runtime": "not_probed",
            "reason": "v1 exposes CPU editing; no model execution or credential discovery is performed"},
        "concurrency": "durable working-head and active-Variant CAS; no live unsaved UI draft arbitration",
        "unsupported": ["named_version_creation", "variant_mutation", "library_batch_editing",
            "new_masks_retouch_paint_liquify", "new_lut_bindings", "raw_foundation_authoring",
            "jpeg_tiff_export", "watermarks", "export_queue_resume", "live_desktop_session"],
        "max_request_bytes": MAX_REQUEST_BYTES, "max_edits": 64
    })
}

fn import(session: &DesktopSession, root: &Path, source: &Path) -> Result<Value> {
    let source = source.canonicalize().context("resolve import source")?;
    if crate::session_pipeline::representation_kind(&source).is_none() {
        return Err(fault(
            "unsupported",
            "source extension is not supported by Shadow",
        ));
    }
    let before = checked_source_fingerprint(&source)?;
    let originals = root.join("originals");
    fs::create_dir_all(&originals).context("create managed original directory")?;
    let mut staging = tempfile::NamedTempFile::new_in(&originals)?;
    let input = fs::File::open(&source)?;
    let copied = std::io::copy(&mut input.take(MAX_SOURCE_BYTES + 1), &mut staging)?;
    if copied != before.byte_len || copied > MAX_SOURCE_BYTES {
        return Err(fault("source_changed", "source size changed during import"));
    }
    staging.flush()?;
    staging.as_file().sync_all()?;
    let digest = shadow_cache::sha256_file(staging.path())?;
    if checked_source_fingerprint(&source)? != before
        || shadow_cache::sha256_file(&source)? != digest
    {
        return Err(fault("source_changed", "source changed during import"));
    }
    let extension = source
        .extension()
        .and_then(|value| value.to_str())
        .ok_or_else(|| fault("unsupported", "source requires a supported extension"))?
        .to_ascii_lowercase();
    let managed = originals.join(format!("{digest}.{extension}"));
    match staging.persist_noclobber(&managed) {
        Ok(_) => (),
        Err(error) if error.error.kind() == std::io::ErrorKind::AlreadyExists => {
            if checked_source_fingerprint(&managed)?.byte_len != copied
                || shadow_cache::sha256_file(&managed)? != digest
            {
                return Err(fault(
                    "source_changed",
                    "managed original no longer matches its content address",
                ));
            }
        }
        Err(error) => return Err(error.error.into()),
    }
    let managed = managed.canonicalize()?;
    let native = native_path_ffi::location_to_ffi(&shadow_core::native_location(&managed))?;
    let receipt = session.admit_pipeline_input(&native)?;
    Ok(
        json!({"photo_id": receipt.photo_id, "representation_id": receipt.representation_id,
        "source_sha256": digest, "managed_original": shadow_core::native_location(&managed)}),
    )
}

fn checked_source_fingerprint(path: &Path) -> Result<shadow_catalog::RepresentationFingerprint> {
    if !fs::metadata(path)?.is_file() {
        return Err(fault("invalid_source", "source must be a regular file"));
    }
    let fingerprint = shadow_core::fingerprint_source(path)?;
    if fingerprint.byte_len == 0 || fingerprint.byte_len > MAX_SOURCE_BYTES {
        return Err(fault(
            "invalid_source",
            "source must contain 1 byte through 2 GiB",
        ));
    }
    Ok(fingerprint)
}

fn verify_source(
    session: &DesktopSession,
    root: &Path,
    photo: &str,
) -> Result<(String, String, String)> {
    let photo: PhotoId = photo.parse().context("parse photo id")?;
    let source = session
        .catalog
        .photo_source(photo)?
        .ok_or_else(|| fault("source_unavailable", "photo has no online original"))?;
    let native = crate::session_photo_source::catalog_native_path(&source)?;
    let path = native.canonicalize()?;
    if path.parent() != Some(root.join("originals").canonicalize()?.as_path()) {
        return Err(fault(
            "unsupported",
            "project commands require their own managed original; use the desktop owner for existing live Catalogs",
        ));
    }
    if checked_source_fingerprint(&path)? != source.source {
        return Err(fault(
            "source_changed",
            "managed original changed since Catalog admission",
        ));
    }
    let digest = shadow_cache::sha256_file(&path)?;
    if path.file_stem().and_then(|value| value.to_str()) != Some(digest.as_str()) {
        return Err(fault(
            "source_changed",
            "managed original bytes do not match their content address",
        ));
    }
    Ok((
        source.location.display_path,
        source.representation_id.to_string(),
        digest,
    ))
}

fn snapshot(
    session: &DesktopSession,
    root: &Path,
    photo: &str,
    base: Option<&str>,
) -> Result<Value> {
    let (source, representation_id, source_sha256) = verify_source(session, root, photo)?;
    let current = session.photo_edit_state(photo, &source)?;
    let state = match base {
        Some(base) => session.checkout_basic_edit_version(photo, &source, base)?,
        None => session.photo_edit_state(photo, &source)?,
    };
    // A concurrent save between the two reads must not pair new content with
    // an old expectation. Rejection is safe; the caller obtains a fresh snapshot.
    if base.is_none() && state.working_commit_id != current.working_commit_id {
        return Err(fault(
            "conflict",
            "working head changed while capturing snapshot",
        ));
    }
    let guard = SnapshotGuard {
        photo_id: photo.to_owned(),
        representation_id,
        source_sha256,
        base_commit_id: state.working_commit_id,
        expected_working_commit_id: current.working_commit_id,
        expected_active_variant_id: current.active_variant_id,
    };
    let record = if guard.base_commit_id.is_empty() {
        None
    } else {
        session
            .catalog
            .recipe_commit(photo.parse()?, guard.base_commit_id.parse()?)?
    };
    Ok(json!({
        "guard": guard, "editable": edit::editable_projection(&state.settings),
        "recipe_snapshot": record.as_ref().map(|record| record.commit.snapshot()),
        "snapshot_digest": record.as_ref().map(|record| crate::digest_hex::encode_hex(&record.snapshot_digest)),
        "recipe_snapshot_role": "read_only_exact_snapshot; commit operations preserve all unaddressed state"
    }))
}

fn load_guard(
    session: &DesktopSession,
    root: &Path,
    guard: &SnapshotGuard,
) -> Result<(String, ffi::FfiEditSettings)> {
    let (source, representation, digest) = verify_source(session, root, &guard.photo_id)?;
    if representation != guard.representation_id || digest != guard.source_sha256 {
        return Err(fault(
            "source_changed",
            "captured representation or original digest no longer matches",
        ));
    }
    let current = session.photo_edit_state(&guard.photo_id, &source)?;
    if current.working_commit_id != guard.expected_working_commit_id
        || current.active_variant_id != guard.expected_active_variant_id
    {
        return Err(fault(
            "conflict",
            "working head or active Variant changed; obtain a new snapshot and reconcile",
        ));
    }
    let settings = if guard.base_commit_id.is_empty() {
        if !guard.expected_working_commit_id.is_empty() {
            return Err(fault(
                "invalid_request",
                "an edited photo requires an explicit content base",
            ));
        }
        current.settings
    } else {
        session
            .checkout_basic_edit_version(&guard.photo_id, &source, &guard.base_commit_id)?
            .settings
    };
    Ok((source, settings))
}

#[cfg(test)]
mod tests;
