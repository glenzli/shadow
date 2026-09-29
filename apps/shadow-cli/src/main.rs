//! Shadow's operator CLI composition boundary.
//!
//! [`commands`] owns argument routing, while each child module owns one durable
//! command family: Catalog status, backups, decode inspection, folder scans,
//! local semantic diagnostics, or authenticated remote Library sharing.

mod backup;
mod catalog;
mod commands;
mod decode;
mod learning;
mod people;
mod remote_library;
mod scan;
mod semantic;

use anyhow::Result;

fn main() -> Result<()> {
    commands::run(std::env::args().skip(1))
}
