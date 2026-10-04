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
mod project;
mod remote_library;
mod scan;
mod semantic;

use anyhow::Result;

fn main() -> Result<()> {
    let arguments = std::env::args_os().skip(1).collect::<Vec<_>>();
    if arguments.first().is_some_and(|value| value == "project") {
        std::process::exit(project::run(&arguments[1..]));
    }
    let arguments = arguments
        .into_iter()
        .map(|value| {
            value
                .into_string()
                .map_err(|_| anyhow::anyhow!("legacy command arguments must be UTF-8"))
        })
        .collect::<Result<Vec<_>>>()?;
    commands::run(arguments)
}
