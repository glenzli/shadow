use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    sync::atomic::Ordering,
};

use anyhow::{Context, Result, bail};
use rusqlite::{Connection, params};
use shadow_native_path::{native_location, native_path_from_location};

use super::{ProfileArchiveControl, manifest, transfer};

pub(super) fn restore(
    source: &Path,
    parent: &Path,
    name: &str,
    control: &ProfileArchiveControl,
) -> Result<(PathBuf, u64, u64)> {
    let name = name.trim();
    if name.is_empty() || name.encode_utf16().count() > 80 || name.chars().any(char::is_control) {
        bail!("use a library name of 1–80 characters without control characters");
    }
    let archive = manifest::verify(source, control)?;
    let old_root = native_path_from_location(&archive.source_root)?;
    let source = source.canonicalize()?;
    if transfer::overlaps_inside(&parent.canonicalize()?, &old_root) {
        bail!("restored library must be outside its original library");
    }
    let directory = transfer::new_directory(parent, &source, "Shadow-Library-")?;
    let root = directory.path();
    control.completed.store(0, Ordering::Release);
    control
        .total
        .store(archive.files.len() as u64, Ordering::Release);
    for entry in &archive.files {
        let relative = manifest::relative_path(&entry.path)?;
        if relative != Path::new("storage-profile.json") {
            transfer::copy(
                &source.join("payload").join(&relative),
                &root.join(relative),
                control,
                Some(&entry.modified),
            )?;
        }
        control.completed.fetch_add(1, Ordering::Release);
    }
    for entry in &archive.files {
        if entry.sqlite {
            let path = root.join(manifest::relative_path(&entry.path)?);
            transfer::verify_database(&path, path == root.join("catalog.sqlite"))?;
        }
    }
    isolate_catalog(&root.join("catalog.sqlite"), &archive.owned_locations, root)?;
    isolate_checkpoint_stores(root)?;
    isolate_settings(&root.join("ui-preferences.ini"))?;
    isolate_settings(&root.join("server-manager.ini"))?;
    // A new library must not inherit an active sharing/remote access identity.
    for relative in [
        "library-server-credentials.ini",
        "remote-library-credentials.ini",
    ] {
        let path = root.join(relative);
        if path.exists() {
            fs::remove_file(path)?;
        }
    }
    shadow_catalog::verify_catalog_backup(&root.join("catalog.sqlite"))?;
    transfer::sync_tree(root, control)?;
    control.check()?;
    let mut marker = archive.profile;
    let old_id = marker["id"].clone();
    marker["id"] = uuid::Uuid::now_v7().to_string().into();
    marker["kind"] = "custom".into();
    marker["displayName"] = name.into();
    marker["restoredFrom"] = serde_json::json!({"profileId":old_id,"sourceRoot":archive.source_root,"pendingWorkDisabled":true});
    // The profile marker is the admission/publication boundary. A crash before
    // this write leaves an unopenable partial directory; existing data is never replaced.
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(root.join("storage-profile.json"))?;
    file.write_all(&serde_json::to_vec_pretty(&marker)?)?;
    file.sync_all()?;
    transfer::sync_directory(root)?;
    transfer::sync_directory(parent)?;
    Ok((
        directory.keep(),
        archive.files.len() as u64,
        archive.byte_count,
    ))
}

fn isolate_catalog(path: &Path, owned: &[manifest::OwnedLocation], new_root: &Path) -> Result<()> {
    let mut connection = Connection::open(path)?;
    connection.execute_batch("PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE;")?;
    let tx = connection.transaction()?;
    for location in owned {
        let relative = manifest::relative_path(&location.relative)?;
        let relocated = native_location(&new_root.join(relative));
        let changed = tx.execute(
            &format!(
                "UPDATE {} SET native_path=?1,display_path=?2 WHERE id=?3",
                location.table
            ),
            params![relocated.native_path, relocated.display_path, location.id],
        )?;
        if changed != 1 {
            bail!("backup owned-location identity is missing");
        }
    }
    // Preserve immutable snapshots/receipts and completed output history. Only
    // nonterminal state changes, so reopening a copy cannot claim old exports.
    tx.execute_batch("UPDATE export_items SET state='cancelled', finished_at_ms=updated_at_ms WHERE state NOT IN ('completed','cancelled','failed');
        UPDATE export_jobs SET state='cancelled', finished_at_ms=updated_at_ms WHERE state NOT IN ('completed','cancelled','failed');
        UPDATE import_sessions SET state='cancelled', finished_at_ms=updated_at_ms WHERE state='running';")?;
    tx.commit()?;
    drop(connection);
    transfer::sync_file(path)?;
    Ok(())
}

fn isolate_checkpoint_stores(root: &Path) -> Result<()> {
    for (relative, sql) in [
        (
            "cache/semantic-index/smart-categories-v1.sqlite3",
            "UPDATE classification_state SET status='paused' WHERE status='running'; UPDATE adaptation_state SET applied_revision=requested_revision;",
        ),
        (
            "cache/image-understanding/proposals-v1.sqlite3",
            "UPDATE run_state SET status='paused' WHERE status='running';",
        ),
    ] {
        let path = root.join(relative);
        if path.exists() {
            let connection = Connection::open(&path)?;
            connection.execute_batch(sql)?;
            drop(connection);
            transfer::sync_file(&path)?;
            transfer::verify_database(&path, false)?;
        }
    }
    Ok(())
}

fn isolate_settings(path: &Path) -> Result<()> {
    let text = if path.exists() {
        fs::read_to_string(path).context("read copied library preferences")?
    } else {
        String::new()
    };
    let disabled = [
        ("ai", "auto_start_enabled"),
        ("ai", "image_understanding_background_enabled"),
        ("ai", "image_understanding_execution_allowed"),
        ("library-server", "auto_start"),
    ];
    let mut section = String::new();
    let mut output = String::new();
    for line in text.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            section = trimmed[1..trimmed.len() - 1].to_owned();
        }
        let key = trimmed.split_once('=').map(|(key, _)| key.trim());
        if disabled
            .iter()
            .any(|(group, field)| section == *group && key == Some(*field))
        {
            continue;
        }
        output.push_str(line);
        output.push('\n');
    }
    output.push_str("\n[ai]\nauto_start_enabled=false\nimage_understanding_background_enabled=false\nimage_understanding_execution_allowed=false\n\n[library-server]\nauto_start=false\n");
    let mut file = fs::File::create(path)?;
    file.write_all(output.as_bytes())?;
    file.sync_all()?;
    Ok(())
}
