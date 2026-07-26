//! Explicit, exact source reattachment for the desktop Library.
//!
//! The UI chooses a historical scan record and a candidate file, but neither
//! file names nor EXIF are trusted to attach anything. This service reads the
//! candidate completely on its worker thread, checks its BLAKE3 identity
//! against the specific catalog representation, then asks the import journal
//! to attach the location atomically.

use std::path::Path;

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::{CatalogHandle, CatalogStore, ImportSessionState, RegisterAsset};
use shadow_core::{
    CatalogRelinkConfirmation, PendingStrongRelink, RelinkSource, StrongRelinkVerification,
    WeakRelinkMetadata, apply_confirmed_relink, confirm_verified_relink, native_location,
    relink_candidate_from_missing_location, verify_pending_relink,
};
use shadow_domain::{ImportSessionId, LocationId, RepresentationKind};

use crate::current_time_ms;

/// A completed exact reattach, intentionally small enough for the CXX bridge.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct VerifiedSourceRelinkReceipt {
    pub photo_id: String,
    pub representation_id: String,
    pub location_id: String,
    pub display_path: String,
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
    /// verification. This operation deliberately does not create a Library
    /// source rooted at the candidate path.
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
        let candidate_path = Path::new(candidate_path)
            .canonicalize()
            .with_context(|| format!("resolve selected source {candidate_path}"))?;
        let now_ms = current_time_ms()?;
        let mut catalog = self.catalog.clone();
        let missing = catalog
            .missing_source_relink_target(scan_session_id, location_id)?
            .ok_or_else(|| {
                anyhow!("the selected location is no longer absent from this completed source scan")
            })?
            .location;
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
            relink_candidate_from_missing_location(&missing),
        );
        let verified = match verify_pending_relink(pending)? {
            StrongRelinkVerification::Verified(verified) => verified,
            StrongRelinkVerification::SourceChanged { .. } => bail!(
                "the selected source changed while Shadow was verifying it; choose it again to retry"
            ),
        };
        let confirmed = match confirm_verified_relink(&catalog, verified)? {
            CatalogRelinkConfirmation::Ready(confirmed) => confirmed,
            CatalogRelinkConfirmation::IdentityNotRecorded { .. } => bail!(
                "this file has no matching exact identity in the catalog; Shadow did not attach it"
            ),
            CatalogRelinkConfirmation::IdentityOwnedByAnotherRepresentation { .. } => bail!(
                "this file exactly matches a different catalog representation; Shadow did not attach it"
            ),
        };

        let session_id = catalog.begin_relocation_session(&request.location, now_ms)?;
        catalog.record_import_discovered(session_id, &request)?;
        let attachment = apply_confirmed_relink(&mut catalog, session_id, &request, &confirmed);
        match attachment {
            Ok(attachment) => {
                catalog.finish_import_session(
                    session_id,
                    ImportSessionState::Completed,
                    None,
                    current_time_ms()?,
                )?;
                Ok(VerifiedSourceRelinkReceipt {
                    photo_id: attachment.photo_id.to_string(),
                    representation_id: attachment.representation_id.to_string(),
                    location_id: attachment.location_id.to_string(),
                    display_path: request.location.display_path,
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

fn system_time_ms(time: std::time::SystemTime) -> Option<i64> {
    let duration = time.duration_since(std::time::UNIX_EPOCH).ok()?;
    i64::try_from(duration.as_millis()).ok()
}
