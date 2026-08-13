//! Desktop-session ownership point for read-only location reference anchors.

use anyhow::{Context, Result as AnyResult, anyhow};

use super::{DesktopSession, ffi};
use crate::{
    location_reference_service::{LocationReferenceLibrary, LocationReferenceSnapshot},
    native_path_ffi::{location_to_ffi, path_from_ffi},
    wall_clock::current_time_ms,
};

impl DesktopSession {
    pub(crate) fn location_reference_libraries(
        &self,
    ) -> AnyResult<Vec<ffi::FfiLocationReferenceLibrary>> {
        self.location_reference_snapshot()?
            .libraries
            .into_iter()
            .map(|library| {
                Ok(ffi::FfiLocationReferenceLibrary {
                    id: library.id,
                    root: location_to_ffi(&library.root)?,
                    clock_offset_seconds: library.clock_offset_seconds,
                    indexed_at_unix_ms: library.indexed_at_unix_ms,
                    anchor_count: library.anchor_count,
                })
            })
            .collect()
    }

    pub(crate) fn location_reference_anchors(
        &self,
        capture_start_unix_seconds: i64,
        capture_end_unix_seconds: i64,
    ) -> AnyResult<Vec<ffi::FfiLocationReferenceAnchor>> {
        Ok(self
            .location_reference_snapshot()?
            .anchors
            .into_iter()
            .filter(|anchor| {
                (capture_start_unix_seconds <= 0
                    || anchor.captured_at_unix_seconds >= capture_start_unix_seconds)
                    && (capture_end_unix_seconds <= 0
                        || anchor.captured_at_unix_seconds <= capture_end_unix_seconds)
            })
            .map(|anchor| ffi::FfiLocationReferenceAnchor {
                library_id: anchor.library_id,
                captured_at_unix_seconds: anchor.captured_at_unix_seconds,
                latitude_e7: anchor.latitude_e7,
                longitude_e7: anchor.longitude_e7,
            })
            .collect())
    }

    pub(crate) fn add_location_reference_library(
        &self,
        root: &ffi::FfiNativePath,
        clock_offset_seconds: i64,
    ) -> AnyResult<ffi::FfiLocationReferenceLibrary> {
        let root = path_from_ffi(root)?;
        let library = self
            .location_references
            .lock()
            .map_err(|_| anyhow!("location reference service lock is poisoned"))
            .context("open location reference library")?
            .add_or_rescan_root(&root, clock_offset_seconds, current_time_ms()?)?;
        ffi_library(library)
    }

    pub(crate) fn remove_location_reference_library(&self, id: &str) -> AnyResult<bool> {
        self.location_references
            .lock()
            .map_err(|_| anyhow!("location reference service lock is poisoned"))
            .context("remove location reference library")?
            .remove(id)
    }

    fn location_reference_snapshot(&self) -> AnyResult<LocationReferenceSnapshot> {
        self.location_references
            .lock()
            .map_err(|_| anyhow!("location reference service lock is poisoned"))
            .context("read location reference anchors")
            .map(|service| service.snapshot())
    }
}

fn ffi_library(library: LocationReferenceLibrary) -> AnyResult<ffi::FfiLocationReferenceLibrary> {
    Ok(ffi::FfiLocationReferenceLibrary {
        id: library.id,
        root: location_to_ffi(&library.root)?,
        clock_offset_seconds: library.clock_offset_seconds,
        indexed_at_unix_ms: library.indexed_at_unix_ms,
        anchor_count: library.anchor_count,
    })
}
