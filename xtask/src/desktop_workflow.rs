//! Canonical desktop build, promotion, and launch workflows.
//!
//! Command routing remains in xtask's facade. Build orchestration, immutable
//! release publication, provider admission, and process launch have separate
//! failure policies and therefore live in responsibility-named children.

mod build;
mod layout;
mod process;
mod promotion;
mod provider;
mod run;

use std::{ffi::OsString, io};

pub(crate) fn build_and_promote(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    build::run(arguments)
}

pub(crate) fn promote(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    promotion::run(arguments)
}

pub(crate) fn launch(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    run::run(arguments)
}

#[cfg(test)]
mod tests;
