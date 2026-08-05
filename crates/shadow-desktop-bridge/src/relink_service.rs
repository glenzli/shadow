//! Folder-owned source reattachment for the desktop Library.
//!
//! Existing BLAKE3 identities always win and permit path-independent matching.
//! Older imports may have no stored identity: after the user chooses a folder,
//! this service admits only one same-name, same-size candidate, reads it
//! completely, and records its first durable identity before attachment. The
//! selected folder is then adopted as a Library source and scanned normally.

use std::path::Path;

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::{
    CatalogHandle, CatalogStore, ImportSessionState, RecordRepresentationContentIdentity,
    RecordRepresentationContentIdentityStatus, RegisterAsset,
};
use shadow_core::{
    CatalogRelinkConfirmation, ConfirmedRelink, PendingStrongRelink, RelinkSource,
    StrongRelinkVerification, WeakRelinkMetadata, apply_confirmed_relink, confirm_verified_relink,
    native_location, native_path_from_location, relink_candidate_from_missing_location,
    verify_pending_relink,
};
use shadow_domain::{ImportSessionId, LibrarySourceId, LocationId, RepresentationKind};

use crate::wall_clock::current_time_ms;

mod folder_recovery;

use folder_recovery::{plan_folder_recovery, plan_source_recovery, same_original_file_name};

/// A completed exact reattach, intentionally small enough for the CXX bridge.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct VerifiedSourceRelinkReceipt {
    pub photo_id: String,
    pub representation_id: String,
    pub location_id: String,
    pub display_path: String,
    pub library_root_path: String,
}

/// Result of locating the missing originals owned by one configured source.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct LibrarySourceRecoveryReceipt {
    pub library_root_path: String,
    pub recovered_photo_count: u64,
    pub unresolved_photo_count: u64,
    pub retired_unavailable_source: bool,
}

/// Stateless facade for the narrow explicit relocation transaction.
#[derive(Debug, Clone)]
pub(crate) struct RelinkService {
    catalog: CatalogHandle,
}

impl RelinkService {
    pub(crate) fn new(catalog: CatalogHandle) -> Self {
        Self { catalog }
    }

    /// Reattaches one user-selected candidate only after complete content
    /// verification. The candidate is never retained as an orphan standalone
    /// source: its containing folder becomes the durable Library root.
    pub(crate) fn relink_missing_source_location(
        &self,
        scan_session_id: &str,
        location_id: &str,
        candidate_path: &str,
    ) -> AnyResult<VerifiedSourceRelinkReceipt> {
        let scan_session_id = scan_session_id
            .trim()
            .parse::<ImportSessionId>()
            .with_context(|| format!("parse source-health scan session id {scan_session_id}"))?;
        let location_id = location_id
            .trim()
            .parse::<LocationId>()
            .with_context(|| format!("parse source-health location id {location_id}"))?;
        let catalog = self.catalog.clone();
        let missing = catalog
            .missing_source_relink_target(scan_session_id, location_id)?
            .ok_or_else(|| {
                anyhow!("the selected location is no longer absent from this completed source scan")
            })?
            .location;
        self.relink_from_user_selection(&missing, candidate_path)
    }

    /// Reattaches the exact historical location projected by a current
    /// Library card. This path is intentionally independent from completed
    /// scan evidence so a disconnected or externally moved folder can be
    /// repaired immediately from the main gallery.
    pub(crate) fn relink_library_source_location(
        &self,
        location_id: &str,
        candidate_path: &str,
    ) -> AnyResult<VerifiedSourceRelinkReceipt> {
        let location_id = location_id
            .trim()
            .parse::<LocationId>()
            .with_context(|| format!("parse Library location id {location_id}"))?;
        let catalog = self.catalog.clone();
        let missing = catalog
            .library_source_relink_target(location_id)?
            .ok_or_else(|| {
                anyhow!("the selected Library source location is no longer available for relinking")
            })?
            .location;
        if native_path_from_location(&missing.location)
            .ok()
            .and_then(|path| std::fs::metadata(path).ok())
            .is_some_and(|metadata| metadata.is_file())
        {
            bail!(
                "the original source is available again; refresh the Library instead of relinking it"
            );
        }
        self.relink_from_user_selection(&missing, candidate_path)
    }

    /// Locates every safely identifiable missing original owned by one
    /// configured source. If the historical root itself is unavailable and
    /// every missing original is recovered, that obsolete discovery root is
    /// disabled after the replacement folder has been adopted.
    pub(crate) fn recover_library_source(
        &self,
        source_id: &str,
        replacement_folder: &str,
    ) -> AnyResult<LibrarySourceRecoveryReceipt> {
        let source_id = source_id
            .trim()
            .parse::<LibrarySourceId>()
            .with_context(|| format!("parse Library source id {source_id}"))?;
        let source = self
            .catalog
            .library_sources()?
            .into_iter()
            .find(|source| source.id == source_id && source.enabled)
            .ok_or_else(|| anyhow!("the selected Library folder is no longer active"))?;
        let replacement_root = Path::new(replacement_folder)
            .canonicalize()
            .with_context(|| format!("resolve selected recovery folder {replacement_folder}"))?;
        if !std::fs::metadata(&replacement_root).is_ok_and(|metadata| metadata.is_dir()) {
            bail!(
                "the selected recovery path is not a folder: {}",
                replacement_root.display()
            );
        }

        let plan = plan_source_recovery(&self.catalog, &source, &replacement_root)?;
        let recovered_photo_count = u64::try_from(plan.recoveries.len()).unwrap_or(u64::MAX);
        let unresolved_photo_count = u64::try_from(
            plan.missing_target_count
                .saturating_sub(plan.recoveries.len()),
        )
        .unwrap_or(u64::MAX);
        for recovery in plan.recoveries {
            self.relink_target(
                &recovery.missing,
                &recovery.path.to_string_lossy(),
                Some(&replacement_root),
                recovery.may_bootstrap_identity,
            )?;
        }
        let retired_unavailable_source = plan.source_root_unavailable
            && unresolved_photo_count == 0
            && self.catalog.remove_library_source(source_id)?;
        Ok(LibrarySourceRecoveryReceipt {
            library_root_path: replacement_root.to_string_lossy().into_owned(),
            recovered_photo_count,
            unresolved_photo_count,
            retired_unavailable_source,
        })
    }

    /// Resolves either the legacy explicitly selected file or the preferred
    /// folder-owned recovery input. Existing exact identities remain the
    /// preferred match. Catalogs imported before those identities were
    /// recorded may recover one uniquely named, same-size original from the
    /// folder the user explicitly chose; that candidate is read completely
    /// and gains a durable identity before it can be attached.
    fn relink_from_user_selection(
        &self,
        missing: &shadow_catalog::MissingSourceLocationRecord,
        selected_path: &str,
    ) -> AnyResult<VerifiedSourceRelinkReceipt> {
        let selected = Path::new(selected_path)
            .canonicalize()
            .with_context(|| format!("resolve selected recovery path {selected_path}"))?;
        let metadata = std::fs::metadata(&selected)
            .with_context(|| format!("inspect selected recovery path {}", selected.display()))?;
        if metadata.is_dir() {
            let recoveries = plan_folder_recovery(&self.catalog, missing, &selected)?;
            let mut selected_receipt = None;
            for recovery in recoveries {
                let receipt = self.relink_target(
                    &recovery.missing,
                    &recovery.path.to_string_lossy(),
                    Some(&selected),
                    recovery.may_bootstrap_identity,
                )?;
                if recovery.missing.location_id == missing.location_id {
                    selected_receipt = Some(receipt);
                }
            }
            selected_receipt.ok_or_else(|| {
                anyhow!("the selected original was not included in the folder recovery plan")
            })
        } else {
            let has_strong_identity = self
                .catalog
                .representation_has_current_whole_file_identity(missing.representation_id)?;
            self.relink_target(
                missing,
                &selected.to_string_lossy(),
                None,
                !has_strong_identity
                    && same_original_file_name(missing, &selected)
                    && metadata.len() == missing.source.byte_len,
            )
        }
    }

    fn relink_target(
        &self,
        missing: &shadow_catalog::MissingSourceLocationRecord,
        candidate_path: &str,
        preferred_library_root: Option<&Path>,
        may_bootstrap_identity: bool,
    ) -> AnyResult<VerifiedSourceRelinkReceipt> {
        let candidate_path = Path::new(candidate_path)
            .canonicalize()
            .with_context(|| format!("resolve selected source {candidate_path}"))?;
        let library_root =
            library_root_for_candidate(&self.catalog, &candidate_path, preferred_library_root)?;
        let now_ms = current_time_ms()?;
        let mut catalog = self.catalog.clone();
        if !matches!(
            missing.kind,
            RepresentationKind::OriginalRaw | RepresentationKind::OriginalRaster
        ) {
            bail!(
                "only original RAW or raster source locations can be reattached; derived artifacts are rebuilt"
            );
        }

        let metadata = std::fs::metadata(&candidate_path)
            .with_context(|| format!("inspect selected source {}", candidate_path.display()))?;
        if !metadata.is_file() {
            bail!(
                "the selected source is not a regular file: {}",
                candidate_path.display()
            );
        }
        let request = RegisterAsset {
            kind: missing.kind,
            location: native_location(&candidate_path),
            byte_len: metadata.len(),
            modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
            now_ms,
        };
        let source = RelinkSource::from_registration(
            candidate_path,
            &request,
            WeakRelinkMetadata::from_path(Path::new(&request.location.display_path)),
        );
        let pending = PendingStrongRelink::explicitly_selected(
            source,
            relink_candidate_from_missing_location(missing),
        );
        let verified = match verify_pending_relink(pending)? {
            StrongRelinkVerification::Verified(verified) => verified,
            StrongRelinkVerification::SourceChanged { .. } => bail!(
                "the selected source changed while Shadow was verifying it; choose it again to retry"
            ),
        };
        let confirmed = confirm_or_bootstrap_recovery_identity(
            &catalog,
            missing,
            confirm_verified_relink(&catalog, verified)?,
            may_bootstrap_identity,
            now_ms,
        )?;

        let library_root_location = native_location(&library_root);
        let session_id = catalog.begin_import_session(&library_root_location, now_ms)?;
        catalog.record_import_discovered(session_id, &request)?;
        let attachment = apply_confirmed_relink(&mut catalog, session_id, &request, &confirmed);
        match attachment {
            Ok(attachment) => {
                // This transaction adopts the folder and attaches the one
                // exactly verified candidate; it is not evidence that the
                // entire folder was enumerated. A cancelled terminal state
                // keeps it out of source-health reconciliation until the
                // desktop immediately runs the ordinary full folder scan.
                catalog.finish_import_session(
                    session_id,
                    ImportSessionState::Cancelled,
                    None,
                    current_time_ms()?,
                )?;
                Ok(VerifiedSourceRelinkReceipt {
                    photo_id: attachment.photo_id.to_string(),
                    representation_id: attachment.representation_id.to_string(),
                    location_id: attachment.location_id.to_string(),
                    display_path: request.location.display_path,
                    library_root_path: library_root_location.display_path,
                })
            }
            Err(error) => {
                let error_message = error.to_string();
                let _ = catalog.finish_import_session(
                    session_id,
                    ImportSessionState::Failed,
                    Some(&error_message),
                    current_time_ms().unwrap_or(now_ms),
                );
                Err(error.into())
            }
        }
    }
}

fn confirm_or_bootstrap_recovery_identity(
    catalog: &CatalogHandle,
    missing: &shadow_catalog::MissingSourceLocationRecord,
    confirmation: CatalogRelinkConfirmation,
    may_bootstrap_identity: bool,
    observed_at_ms: i64,
) -> AnyResult<ConfirmedRelink> {
    let verified = match confirmation {
        CatalogRelinkConfirmation::Ready(confirmed) => return Ok(confirmed),
        CatalogRelinkConfirmation::IdentityNotRecorded { verified } => verified,
        CatalogRelinkConfirmation::IdentityOwnedByAnotherRepresentation { .. } => bail!(
            "this file exactly matches a different catalog representation; Shadow did not attach it"
        ),
    };
    if !may_bootstrap_identity {
        bail!(
            "the selected folder did not contain a uniquely matching original; Shadow did not attach it"
        );
    }
    let status =
        catalog.record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: missing.representation_id,
            expected_source: missing.source,
            identity: verified.identity.clone(),
            observed_at_ms,
        })?;
    if status == RecordRepresentationContentIdentityStatus::StaleSource {
        bail!(
            "the catalog source changed while Shadow was verifying the selected original; refresh and try again"
        );
    }
    match confirm_verified_relink(catalog, verified)? {
        CatalogRelinkConfirmation::Ready(confirmed) => Ok(confirmed),
        CatalogRelinkConfirmation::IdentityNotRecorded { .. } => {
            bail!("Shadow could not persist the verified original identity; nothing was attached")
        }
        CatalogRelinkConfirmation::IdentityOwnedByAnotherRepresentation { .. } => bail!(
            "this file exactly matches a different catalog representation; Shadow did not attach it"
        ),
    }
}

fn library_root_for_candidate(
    catalog: &CatalogHandle,
    candidate: &Path,
    preferred_library_root: Option<&Path>,
) -> AnyResult<std::path::PathBuf> {
    let existing_root = catalog
        .library_sources()?
        .into_iter()
        .filter_map(|source| native_path_from_location(&source.root).ok())
        .filter_map(|root| root.canonicalize().ok())
        .filter(|root| candidate.starts_with(root))
        .max_by_key(|root| root.components().count());
    existing_root.map_or_else(
        || {
            preferred_library_root.map_or_else(
                || {
                    candidate
                        .parent()
                        .map(Path::to_path_buf)
                        .ok_or_else(|| anyhow!("the selected source has no containing folder"))
                },
                |root| Ok(root.to_path_buf()),
            )
        },
        Ok,
    )
}

fn system_time_ms(time: std::time::SystemTime) -> Option<i64> {
    let duration = time.duration_since(std::time::UNIX_EPOCH).ok()?;
    i64::try_from(duration.as_millis()).ok()
}

#[cfg(test)]
mod tests;
