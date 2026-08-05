//! Lightweight Library-server process host for the standalone operator app.
//!
//! The normal desktop session owns many unrelated Catalog and editing services. A dedicated
//! server operator needs only the managed listener lifecycle and its private storage root, so this
//! owner keeps that process boundary independent while reusing the exact same server service.

use std::path::Path;

use anyhow::{Context, Error, Result};

use crate::session_library_server::{project_snapshot, start_request};
use crate::{LibraryServerService, LibraryServerStorage, ffi};

#[derive(Debug)]
pub(crate) struct LibraryServerHost {
    service: LibraryServerService,
}

pub(crate) fn open_library_server_host(storage_root: &str) -> Result<Box<LibraryServerHost>> {
    let storage_root = Path::new(storage_root);
    std::fs::create_dir_all(storage_root).with_context(|| {
        format!(
            "create standalone Library server storage {}",
            storage_root.display()
        )
    })?;
    Ok(Box::new(LibraryServerHost {
        service: LibraryServerService::new(LibraryServerStorage::for_root(storage_root)),
    }))
}

impl LibraryServerHost {
    pub(crate) fn snapshot(&self) -> Result<ffi::FfiLibraryServerSnapshot> {
        self.service
            .snapshot()
            .map(project_snapshot)
            .map_err(|error| expanded_error(&error))
    }

    pub(crate) fn start(
        &self,
        config: &ffi::FfiLibraryServerConfig,
    ) -> Result<ffi::FfiLibraryServerSnapshot> {
        let request = start_request(config).map_err(|error| expanded_error(&error))?;
        self.service
            .start(request)
            .map(project_snapshot)
            .map_err(|error| expanded_error(&error))
    }

    pub(crate) fn stop(&self) -> Result<ffi::FfiLibraryServerSnapshot> {
        self.service
            .stop()
            .map(project_snapshot)
            .map_err(|error| expanded_error(&error))
    }

    pub(crate) fn reset_cache(&self) -> Result<ffi::FfiLibraryServerSnapshot> {
        self.service
            .reset_cache()
            .map(project_snapshot)
            .map_err(|error| expanded_error(&error))
    }
}

fn expanded_error(error: &Error) -> Error {
    Error::msg(format!("{error:#}"))
}
