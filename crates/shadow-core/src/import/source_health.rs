//! Non-mutating bridge from source-health review records to safe reattach.
//!
//! A missing-source page only says that a location was absent from one scan.
//! This module deliberately does not promote that observation into a catalog
//! mutation. It turns the page's weak facts into the existing staged relink
//! contract, performs a full-file verification only for a unique candidate,
//! and returns an explicit catalog confirmation for a later import-journal
//! attach.

use shadow_catalog::{CatalogError, MissingSourceLocationRecord};
use thiserror::Error;

use super::{
    CatalogRelinkConfirmation, PendingStrongRelink, RelinkCandidate, RelinkIdentityLookup,
    RelinkSource, RelinkVerificationError, SourceRelinkResolution, StrongRelinkVerification,
    UnsupportedRelinkSource, WeakRelinkMetadata, confirm_verified_relink, discover_source_relink,
    verify_pending_relink,
};

/// The non-mutating outcome of attempting one safe reattach from a source
/// health review page.
///
/// `Catalog` includes both a ready proof and the explicit cases where a full
/// identity was absent or belonged to another representation. Only a caller
/// holding `CatalogStore` plus an import-journal discovery entry may turn a
/// `Ready` confirmation into an attach.
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum SafeReattachPlan {
    Unsupported(UnsupportedRelinkSource),
    NoLikelyCandidate,
    Ambiguous {
        candidates: Vec<RelinkCandidate>,
    },
    SourceChanged {
        pending: PendingStrongRelink,
        observed_before: shadow_catalog::RepresentationFingerprint,
        observed_after: Option<shadow_catalog::RepresentationFingerprint>,
    },
    Catalog(CatalogRelinkConfirmation),
}

/// A read-only reattach plan can fail only while hashing the discovered file
/// or asking the catalog who owns its exact identity.
#[derive(Debug, Error)]
pub enum SafeReattachPlanningError {
    #[error("cannot strongly verify the discovered source: {0}")]
    Verification(#[from] RelinkVerificationError),
    #[error("cannot confirm the strongly verified identity in the catalog: {0}")]
    Catalog(#[from] CatalogError),
}

/// Converts one source-health record to the existing weak-candidate contract.
///
/// The location label and recovered filename are presentation-only weak
/// observations. Exact identity remains the complete BLAKE3 verifier used by
/// [`plan_safe_reattach`].
#[must_use]
pub fn relink_candidate_from_missing_location(
    location: &MissingSourceLocationRecord,
) -> RelinkCandidate {
    RelinkCandidate {
        photo_id: location.photo_id,
        representation_id: location.representation_id,
        kind: location.kind,
        fingerprint: location.source,
        metadata: WeakRelinkMetadata {
            file_name: display_file_name(&location.location.display_path),
            captured_at_unix_seconds: location.captured_at_unix_seconds,
            camera_key: nonempty(&location.camera_key),
        },
        location_label: location.location.display_path.clone(),
    }
}

/// Builds a safe reattach plan without writing to the catalog.
///
/// Call this from a worker rather than the interactive thread: the unique
/// candidate path is read completely to produce a whole-file BLAKE3 identity.
/// Ambiguous/no-match paths intentionally skip that I/O. The returned plan is
/// still only a proof; use the existing explicit `apply_confirmed_relink`
/// operation after a fresh scan has journaled the discovered source.
///
/// # Errors
///
/// Returns an error when the unique discovered source cannot be read stably,
/// or when its exact identity cannot be checked against the catalog.
pub fn plan_safe_reattach(
    catalog: &impl RelinkIdentityLookup,
    source: RelinkSource,
    missing_locations: impl IntoIterator<Item = MissingSourceLocationRecord>,
) -> Result<SafeReattachPlan, SafeReattachPlanningError> {
    let candidates = missing_locations
        .into_iter()
        .map(|location| relink_candidate_from_missing_location(&location));
    match discover_source_relink(source, candidates) {
        SourceRelinkResolution::Unsupported(reason) => Ok(SafeReattachPlan::Unsupported(reason)),
        SourceRelinkResolution::NoLikelyCandidate => Ok(SafeReattachPlan::NoLikelyCandidate),
        SourceRelinkResolution::Ambiguous { candidates } => {
            Ok(SafeReattachPlan::Ambiguous { candidates })
        }
        SourceRelinkResolution::VerificationRequired(pending) => {
            match verify_pending_relink(*pending)? {
                StrongRelinkVerification::SourceChanged {
                    pending,
                    observed_before,
                    observed_after,
                } => Ok(SafeReattachPlan::SourceChanged {
                    pending,
                    observed_before,
                    observed_after,
                }),
                StrongRelinkVerification::Verified(verified) => Ok(SafeReattachPlan::Catalog(
                    confirm_verified_relink(catalog, verified)?,
                )),
            }
        }
    }
}

fn display_file_name(display_path: &str) -> Option<String> {
    display_path.rsplit(['/', '\\']).next().and_then(nonempty)
}

fn nonempty(value: &str) -> Option<String> {
    let value = value.trim();
    (!value.is_empty()).then(|| value.to_owned())
}

#[cfg(test)]
mod tests {
    use std::{fs, path::Path, time::UNIX_EPOCH};

    use shadow_catalog::{
        Catalog, ContentIdentity, RecordRepresentationContentIdentity, RegisterAsset,
        RepresentationFingerprint,
    };
    use shadow_domain::{
        AssetLocation, EntityId, LocationId, PhotoId, Platform, RepresentationKind,
    };

    use super::*;

    fn fingerprint(path: &Path) -> RepresentationFingerprint {
        let metadata = fs::metadata(path).expect("read fixture metadata");
        RepresentationFingerprint {
            byte_len: metadata.len(),
            modified_at_ms: metadata.modified().ok().and_then(|time| {
                time.duration_since(UNIX_EPOCH)
                    .ok()
                    .and_then(|duration| i64::try_from(duration.as_millis()).ok())
            }),
        }
    }

    fn missing_location(
        photo_id: PhotoId,
        representation_id: shadow_domain::RepresentationId,
        source: RepresentationFingerprint,
    ) -> MissingSourceLocationRecord {
        MissingSourceLocationRecord {
            photo_id,
            representation_id,
            location_id: LocationId::new_v7(),
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::OtherUnix,
                b"/former-drive/DCIM/DSC_0001.NEF".to_vec(),
                "/former-drive/DCIM/DSC_0001.NEF",
            ),
            source,
            captured_at_unix_seconds: Some(1_700_000_000),
            camera_key: "nikon z9".to_owned(),
            last_seen_at_ms: 10,
        }
    }

    #[test]
    fn missing_location_conversion_keeps_only_weak_presentation_evidence() {
        let source = RepresentationFingerprint {
            byte_len: 42,
            modified_at_ms: Some(7),
        };
        let location = missing_location(
            PhotoId::new_v7(),
            shadow_domain::RepresentationId::new_v7(),
            source,
        );

        let candidate = relink_candidate_from_missing_location(&location);
        assert_eq!(candidate.fingerprint, source);
        assert_eq!(
            candidate.metadata.file_name.as_deref(),
            Some("DSC_0001.NEF")
        );
        assert_eq!(candidate.metadata.camera_key.as_deref(), Some("nikon z9"));
        assert_eq!(candidate.location_label, location.location.display_path);
    }

    #[test]
    fn unique_missing_location_can_produce_a_ready_read_only_plan() {
        let path =
            std::env::temp_dir().join(format!("shadow-source-health-{}.nef", PhotoId::new_v7()));
        let bytes = b"same original bytes";
        fs::write(&path, bytes).expect("write fixture");
        let source_fingerprint = fingerprint(&path);

        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let original = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::OtherUnix,
                    b"/former-drive/DCIM/DSC_0001.NEF".to_vec(),
                    "/former-drive/DCIM/DSC_0001.NEF",
                ),
                byte_len: source_fingerprint.byte_len,
                modified_at_ms: source_fingerprint.modified_at_ms,
                now_ms: 1,
            })
            .expect("register old location");
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: source_fingerprint,
                identity: ContentIdentity::whole_file_blake3(*blake3::hash(bytes).as_bytes()),
                observed_at_ms: 2,
            })
            .expect("record identity");

        let source = RelinkSource {
            path: path.clone(),
            kind: RepresentationKind::OriginalRaw,
            fingerprint: source_fingerprint,
            metadata: WeakRelinkMetadata {
                file_name: Some("DSC_0001.NEF".to_owned()),
                captured_at_unix_seconds: Some(1_700_000_000),
                camera_key: Some("nikon z9".to_owned()),
            },
        };
        let plan = plan_safe_reattach(
            &catalog,
            source,
            [missing_location(
                original.photo_id,
                original.representation_id,
                source_fingerprint,
            )],
        )
        .expect("plan source reattach");

        let SafeReattachPlan::Catalog(CatalogRelinkConfirmation::Ready(confirmed)) = plan else {
            panic!("exact verified identity should be ready, not attached");
        };
        assert_eq!(
            confirmed.matched.representation_id,
            original.representation_id
        );
        assert_eq!(catalog.stats().expect("stats").locations, 1);
        fs::remove_file(path).expect("remove fixture");
    }
}
