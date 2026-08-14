//! Canonical desktop build, promotion, and launch workflows.
//!
//! Command routing remains in xtask's facade. Build orchestration, immutable
//! release publication, provider admission, and process launch have separate
//! failure policies and therefore live in responsibility-named children.

mod build;
mod layout;
mod lock;
mod process;
mod promotion;
mod provider;
mod run;

use std::{ffi::OsString, io, path::PathBuf};

pub(crate) fn build_and_promote(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    build::run(arguments)
}

pub(crate) fn promote(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    promotion::run(arguments)
}

pub(crate) fn launch(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    run::run(arguments)
}

pub(crate) fn canonical_debug_storage_paths() -> io::Result<(PathBuf, PathBuf)> {
    let paths = layout::WorkflowPaths::resolve()?;
    Ok((paths.repository_root, paths.local_build_root))
}

pub(crate) fn acquire_canonical_debug_lock() -> io::Result<lock::CanonicalDebugLock> {
    let paths = layout::WorkflowPaths::resolve()?;
    lock::CanonicalDebugLock::acquire(&paths.repository_root, &paths.local_build_root)
}

#[cfg(test)]
mod tests;
