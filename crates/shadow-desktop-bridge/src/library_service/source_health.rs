//! Read-only Library source-health and missing-location projections.

use anyhow::{Context, Result as AnyResult};
use shadow_catalog::{LibrarySourceHealth, MissingSourceLocationCursor, MissingSourceLocationPage};
use shadow_domain::{ImportSessionId, LibrarySourceId, LocationId};

use crate::{ffi, review_service::file_name};

use super::LibraryService;

impl LibraryService {
    /// Removes only the configured discovery root. Catalog locations, photos,
    /// edits, and files on disk are deliberately outside this mutation.
    pub(crate) fn remove_source(&self, source_id: &str) -> AnyResult<bool> {
        let source_id = source_id
            .trim()
            .parse::<LibrarySourceId>()
            .with_context(|| format!("parse Library source id {source_id}"))?;
        Ok(self.catalog.remove_library_source(source_id)?)
    }

    /// Projects only observational source-scan evidence. The desktop must
    /// never infer a global missing file or a replacement path from this list.
    pub(crate) fn ffi_source_health(&self) -> AnyResult<Vec<ffi::FfiLibrarySourceHealth>> {
        Ok(self
            .catalog
            .library_source_health()?
            .into_iter()
            .map(ffi_library_source_health)
            .collect())
    }

    /// Reads a bounded page of locations absent from one completed source
    /// scan. This remains strictly observational: reattach planning and any
    /// later confirmation live behind a separate, explicit workflow.
    pub(crate) fn ffi_missing_source_location_page(
        &self,
        scan_session_id: &str,
        after_location_id: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiMissingSourceLocationPage> {
        let scan_session_id = scan_session_id
            .trim()
            .parse::<ImportSessionId>()
            .with_context(|| format!("parse source-health scan session id {scan_session_id}"))?;
        let after = if after_location_id.trim().is_empty() {
            None
        } else {
            Some(MissingSourceLocationCursor {
                location_id: after_location_id
                    .trim()
                    .parse::<LocationId>()
                    .with_context(|| {
                        format!("parse source-health location cursor {after_location_id}")
                    })?,
            })
        };
        let page = self.catalog.missing_source_location_page(
            scan_session_id,
            after.as_ref(),
            usize::try_from(limit).unwrap_or(usize::MAX),
        )?;
        Ok(page.map_or_else(
            empty_ffi_missing_source_location_page,
            ffi_missing_source_location_page,
        ))
    }
}

fn ffi_library_source_health(health: LibrarySourceHealth) -> ffi::FfiLibrarySourceHealth {
    let source_id = health.source.id.to_string();
    let source_display_path = health.source.root.display_path;
    let source_enabled = health.source.enabled;
    let (
        has_latest_completed_scan,
        scan_session_id,
        scan_completed_at_ms,
        known_locations,
        seen_locations,
        not_seen_locations,
    ) = match health.latest_completed_scan {
        Some(scan) => (
            true,
            scan.session_id.to_string(),
            scan.completed_at_ms,
            scan.known_locations,
            scan.seen_locations,
            scan.not_seen_locations,
        ),
        None => (false, String::new(), 0, 0, 0, 0),
    };
    ffi::FfiLibrarySourceHealth {
        source_id,
        source_display_path,
        source_enabled,
        has_latest_completed_scan,
        scan_session_id,
        scan_completed_at_ms,
        known_locations,
        seen_locations,
        not_seen_locations,
    }
}

fn empty_ffi_missing_source_location_page() -> ffi::FfiMissingSourceLocationPage {
    ffi::FfiMissingSourceLocationPage {
        has_scan: false,
        items: Vec::new(),
        has_more: false,
        next_location_id: String::new(),
    }
}

fn ffi_missing_source_location_page(
    page: MissingSourceLocationPage,
) -> ffi::FfiMissingSourceLocationPage {
    let next_location_id = page
        .next_cursor
        .map_or_else(String::new, |cursor| cursor.location_id.to_string());
    ffi::FfiMissingSourceLocationPage {
        has_scan: true,
        items: page
            .items
            .into_iter()
            .map(|item| ffi::FfiMissingSourceLocation {
                location_id: item.location_id.to_string(),
                photo_id: item.photo_id.to_string(),
                title: file_name(&item.location.display_path),
                source_display_path: item.location.display_path,
                has_captured_at: item.captured_at_unix_seconds.is_some(),
                captured_at_unix_seconds: item.captured_at_unix_seconds.unwrap_or_default(),
                camera_key: item.camera_key,
                last_seen_at_ms: item.last_seen_at_ms,
            })
            .collect(),
        has_more: !next_location_id.is_empty(),
        next_location_id,
    }
}
