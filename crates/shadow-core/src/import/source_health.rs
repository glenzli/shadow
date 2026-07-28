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
mod tests;
