//! Reversible Library source removal, source-health, and missing-location projections.

use std::collections::HashSet;

use anyhow::{Context, Result as AnyResult, bail};
use shadow_catalog::{
    LibrarySourceHealth, LibrarySourceRecord, MissingSourceLocationCursor,
    MissingSourceLocationPage,
};
use shadow_core::{native_location, native_path_from_location};
use shadow_domain::{ImportSessionId, LibrarySourceId, LocationId, PhotoId};

use crate::{ffi, review_service::file_name, wall_clock::current_time_ms};

use super::LibraryService;

const MAX_SOURCE_RECONCILIATION_PHOTOS: usize = 4_096;

/// Result of explicitly removing photos that remained unavailable after one
/// completed source scan and an immediate all-location filesystem check.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) struct SourceReconciliationReceipt {
    pub reviewed: u64,
    pub archived: u64,
    pub retained_available: u64,
}

impl LibraryService {
    /// Removes only the configured discovery root. Catalog locations, photos,
    /// edits, and files on disk are deliberately outside this mutation.
    ///
    /// Before disabling the source, equivalent current-filesystem roots are
    /// supplied to the Catalog so source-less imports made by older Shadow
    /// versions follow the same reversible visibility lifecycle.
    pub(crate) fn remove_source(&self, source_id: &str) -> AnyResult<bool> {
        let source_id = source_id
            .trim()
            .parse::<LibrarySourceId>()
            .with_context(|| format!("parse Library source id {source_id}"))?;
        let source = self
            .catalog
            .library_sources()?
            .into_iter()
            .find(|source| source.id == source_id);
        let Some(source) = source else {
            return Ok(false);
        };
        Ok(self.catalog.remove_library_source_with_legacy_roots(
            source_id,
            legacy_roots_for_source(&source),
            current_time_ms()?,
        )?)
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

    /// Archives only scan-missing photos for which every retained original
    /// location is still unavailable at confirmation time. This preserves a
    /// photo that remains reachable through an overlapping source or a second
    /// path, even when one source-specific scan did not see it.
    pub(crate) fn reconcile_missing_source_photos(
        &self,
        scan_session_id: &str,
    ) -> AnyResult<SourceReconciliationReceipt> {
        let scan_session_id = scan_session_id
            .trim()
            .parse::<ImportSessionId>()
            .with_context(|| format!("parse source-health scan session id {scan_session_id}"))?;
        let mut cursor = None;
        let mut photo_ids = HashSet::<PhotoId>::new();
        loop {
            let Some(page) =
                self.catalog
                    .missing_source_location_page(scan_session_id, cursor.as_ref(), 256)?
            else {
                bail!("the selected source scan is no longer available for reconciliation");
            };
            for item in page.items {
                photo_ids.insert(item.photo_id);
                if photo_ids.len() > MAX_SOURCE_RECONCILIATION_PHOTOS {
                    bail!(
                        "source reconciliation is limited to {MAX_SOURCE_RECONCILIATION_PHOTOS} photos"
                    );
                }
            }
            cursor = page.next_cursor;
            if cursor.is_none() {
                break;
            }
        }

        let mut archived_photo_count = 0_u64;
        let mut retained_available_photo_count = 0_u64;
        for photo_id in &photo_ids {
            let locations = self.catalog.library_photo_original_locations(*photo_id)?;
            let definitely_unavailable = !locations.is_empty()
                && locations.iter().all(|location| {
                    let Ok(path) = native_path_from_location(location) else {
                        // A location from another platform cannot be disproved by
                        // this machine. Retain the photo rather than treating a
                        // path-decoding failure as evidence that the original is
                        // gone.
                        return false;
                    };
                    std::fs::metadata(path).map_or(true, |metadata| !metadata.is_file())
                });
            if definitely_unavailable {
                archived_photo_count += u64::from(self.catalog.archive_library_photo(*photo_id)?);
            } else {
                retained_available_photo_count += 1;
            }
        }
        Ok(SourceReconciliationReceipt {
            reviewed: u64::try_from(photo_ids.len()).unwrap_or(u64::MAX),
            archived: archived_photo_count,
            retained_available: retained_available_photo_count,
        })
    }
}

fn legacy_roots_for_source(source: &LibrarySourceRecord) -> Vec<shadow_domain::AssetLocation> {
    let mut roots = vec![source.root.clone()];
    let Ok(native_root) = native_path_from_location(&source.root) else {
        return roots;
    };
    if let Ok(link_target) = std::fs::read_link(&native_root) {
        let resolved_target = if link_target.is_absolute() {
            link_target
        } else {
            native_root
                .parent()
                .map_or(link_target.clone(), |parent| parent.join(link_target))
        };
        push_distinct_root(&mut roots, native_location(&resolved_target));
    }
    let Ok(canonical_root) = std::fs::canonicalize(native_root) else {
        return roots;
    };
    push_distinct_root(&mut roots, native_location(&canonical_root));
    roots
}

fn push_distinct_root(
    roots: &mut Vec<shadow_domain::AssetLocation>,
    candidate: shadow_domain::AssetLocation,
) {
    if !roots.iter().any(|root| {
        root.platform == candidate.platform && root.native_path == candidate.native_path
    }) {
        roots.push(candidate);
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

#[cfg(test)]
mod tests;
