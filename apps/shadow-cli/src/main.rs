//! Shadow's operator CLI composition boundary.
//!
//! [`commands`] owns argument routing, while each child module owns one durable
//! command family: Catalog status, backups, decode inspection, or folder scans.

mod backup;
mod catalog;
mod commands;
mod decode;
mod scan;

use anyhow::Result;

fn main() -> Result<()> {
    commands::run(std::env::args().skip(1))
}
