//! Verified whole-profile snapshots and non-destructive, independently owned restores.
//!
//! Qt owns writer admission and saving the current draft. This owner snapshots
//! every SQLite database with the online backup API, verifies attachment bytes,
//! and publishes only complete directories. It never opens or copies external
//! original photographs. Follow `transfer` for bounded I/O, `manifest` for the
//! archive contract, and `fork` for new identity and runnable-work isolation.

mod fork;
mod manifest;
mod transfer;

use std::{
    path::Path,
    sync::atomic::{AtomicBool, AtomicU64, Ordering},
};

use anyhow::{Context, Result, bail};

use crate::{ffi, native_path_ffi};

#[derive(Debug, Default)]
pub struct ProfileArchiveControl {
    cancelled: AtomicBool,
    completed: AtomicU64,
    total: AtomicU64,
}

impl ProfileArchiveControl {
    fn check(&self) -> Result<()> {
        if self.cancelled.load(Ordering::Acquire) {
            bail!("library operation cancelled");
        }
        Ok(())
    }
}

pub(crate) fn new_profile_archive_control() -> Box<ProfileArchiveControl> {
    Box::default()
}

pub(crate) fn cancel_profile_archive(control: &ProfileArchiveControl) {
    control.cancelled.store(true, Ordering::Release);
}

pub(crate) fn profile_archive_progress(
    control: &ProfileArchiveControl,
) -> ffi::FfiProfileArchiveProgress {
    ffi::FfiProfileArchiveProgress {
        completed_files: control.completed.load(Ordering::Acquire),
        total_files: control.total.load(Ordering::Acquire),
    }
}

pub(crate) fn run_profile_archive(
    action: u8,
    source: &ffi::FfiNativePath,
    parent: &ffi::FfiNativePath,
    name: &str,
    expected_id: &str,
    control: &ProfileArchiveControl,
) -> Result<ffi::FfiProfileArchiveResult> {
    let source = native_path_ffi::path_from_ffi(source)?;
    let parent = native_path_ffi::path_from_ffi(parent)?;
    let (path, count, bytes) = run(action, &source, &parent, name, expected_id, control)?;
    Ok(ffi::FfiProfileArchiveResult {
        root: native_path_ffi::location_to_ffi(&shadow_native_path::native_location(&path))?,
        file_count: count,
        byte_count: bytes,
    })
}

fn run(
    action: u8,
    source: &Path,
    parent: &Path,
    name: &str,
    expected_id: &str,
    control: &ProfileArchiveControl,
) -> Result<(std::path::PathBuf, u64, u64)> {
    control.check()?;
    match action {
        0 => {
            let (directory, manifest) = transfer::snapshot(source, parent, expected_id, control)?;
            let count = manifest.files.len() as u64;
            Ok((directory.keep(), count, manifest.byte_count))
        }
        1 => fork::restore(source, parent, name, control),
        2 => {
            // The intermediate verified snapshot is private and removed on
            // every terminal path. The source remains owned by Qt throughout.
            let (snapshot, _) = transfer::snapshot(source, parent, expected_id, control)?;
            fork::restore(snapshot.path(), parent, name, control)
        }
        3 => {
            let manifest = manifest::verify(source, control)?;
            Ok((
                source.canonicalize()?,
                manifest.files.len() as u64,
                manifest.byte_count,
            ))
        }
        _ => bail!("unsupported library operation"),
    }
    .context("library data protection")
}

#[cfg(test)]
mod tests;
