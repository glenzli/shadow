//! End-to-end acceptance smoke for Shadow's normal local workflow.
//!
//! This is the acceptance command boundary and navigation index:
//!
//! - [`desktop`] owns packaged-app construction and the normal edit workflow.
//! - [`extended`] owns slower hardware-local baselines.
//! - [`fixtures`] owns bounded RAW/DNG discovery.
//! - [`process`] owns child-process evidence and runtime diagnostic admission.
//! - [`session`] owns the short-lived persisted Catalog/cache root.
//!
//! The workflow deliberately orchestrates the desktop application's existing
//! smoke switches instead of duplicating controller behavior in Rust.

mod desktop;
mod extended;
mod fixtures;
mod process;
mod session;

use std::{ffi::OsString, io, path::PathBuf};

pub fn run(fixture_argument: Option<OsString>) -> io::Result<()> {
    let repository_root = repository_root();
    let fixture_directory =
        fixtures::resolve_fixture_directory(&repository_root, fixture_argument)?;
    let extended = extended::enabled();

    desktop::build_release(&repository_root)?;
    let executable = desktop::release_executable(&repository_root)?;
    if !executable.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "desktop build completed but the app executable is missing: {}",
                executable.display()
            ),
        ));
    }

    let session = session::AcceptanceSession::create()?;
    println!(
        "daily-use smoke: fixture={} data-root={}",
        fixture_directory.display(),
        session.data_root().display()
    );

    let result = desktop::run_scenario(&executable, &fixture_directory, session.data_root())
        .and_then(|()| {
            if extended {
                extended::run(&repository_root, &fixture_directory, session.data_root())
            } else {
                Ok(())
            }
        });
    match result {
        Ok(()) => {
            session.remove()?;
            println!("daily-use smoke passed");
            Ok(())
        }
        Err(error) => {
            eprintln!(
                "daily-use smoke failed; preserving its Catalog/cache for inspection: {}",
                session.data_root().display()
            );
            Err(error)
        }
    }
}

fn repository_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .to_path_buf()
}
