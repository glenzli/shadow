//! Safe, staged discovery for files that may have moved outside Shadow.
//!
//! A filesystem path is deliberately not photo identity. However, weak
//! observations such as a filename, byte length, capture time, and camera
//! model are not strong enough to merge two records either. This module keeps
//! those two facts separate:
//!
//! 1. weak observations only select a *single* candidate worth verifying;
//! 2. a complete BLAKE3 file hash verifies the current source did not change
//!    while it was read; and
//! 3. a catalog identity lookup may then confirm that the exact identity still
//!    belongs to that candidate.
//!
//! Discovery and verification intentionally never write to the catalog.
//! [`apply_confirmed_relink`] is the one explicit mutation boundary: it hands
//! a confirmed identity to the catalog-side transaction, which still proves
//! the target location is new and journals the attach atomically.

use std::{
    collections::HashSet,
    fs::{self, File},
    io::{BufReader, Read},
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_catalog::{
    Catalog, CatalogError, CatalogHandle, CatalogStore, ContentIdentity, RegisterAsset,
    RegisteredAsset, RelinkMatch, RepresentationFingerprint,
};
use shadow_domain::{ImportSessionId, PhotoId, RepresentationId, RepresentationKind};
use thiserror::Error;

use crate::native_path::native_location;

/// Low-cost source facts used only to identify a possible relocation
/// candidate. None of these values can merge records on their own.
#[derive(Debug, Clone, Eq, PartialEq, Default)]
pub struct WeakRelinkMetadata {
    /// Camera-created filename when available, for example `DSC_1234.NEF`.
    pub file_name: Option<String>,
    /// Capture timestamp from EXIF/XMP, in Unix seconds.
    pub captured_at_unix_seconds: Option<i64>,
    /// Normalized make/model key when a metadata stage has produced one.
    pub camera_key: Option<String>,
}

impl WeakRelinkMetadata {
    /// Creates metadata with the filename recovered from a source path.
    #[must_use]
    pub fn from_path(path: &Path) -> Self {
        Self {
            file_name: path
                .file_name()
                .and_then(|name| name.to_str())
                .map(str::to_owned),
            ..Self::default()
        }
    }
}

/// One newly discovered source together with its registration fingerprint.
///
/// `fingerprint` should come from the scanner's `metadata()` call. The strong
/// verifier reads it again before and after hashing so a changed file is never
/// offered to the catalog as a relocation.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RelinkSource {
    pub path: PathBuf,
    pub kind: RepresentationKind,
    pub fingerprint: RepresentationFingerprint,
    pub metadata: WeakRelinkMetadata,
}

impl RelinkSource {
    /// Builds a relocation source from the same registration values used by
    /// the normal importer. This preserves current import identity semantics;
    /// relocation is an optional later stage, not an alternative registration
    /// format.
    #[must_use]
    pub fn from_registration(
        path: impl Into<PathBuf>,
        registration: &RegisterAsset,
        metadata: WeakRelinkMetadata,
    ) -> Self {
        Self {
            path: path.into(),
            kind: registration.kind,
            fingerprint: RepresentationFingerprint {
                byte_len: registration.byte_len,
                modified_at_ms: registration.modified_at_ms,
            },
            metadata,
        }
    }
}

/// One pre-existing representation offered by a Library/catalog projection as
/// a possible moved-source match.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RelinkCandidate {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub kind: RepresentationKind,
    pub fingerprint: RepresentationFingerprint,
    pub metadata: WeakRelinkMetadata,
    /// Presentation-only path label for diagnostics and later UI review.
    /// It never participates in identity or catalog writes.
    pub location_label: String,
}

/// Which low-cost fields agreed for a candidate. This is diagnostic evidence,
/// not a confidence score and never authorizes a catalog merge.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct WeakRelinkEvidence(u8);

impl WeakRelinkEvidence {
    const FILE_NAME: u8 = 1 << 0;
    const BYTE_LEN: u8 = 1 << 1;
    const CAPTURE_TIME: u8 = 1 << 2;
    const CAMERA: u8 = 1 << 3;

    const fn from_bits(bits: u8) -> Self {
        Self(bits)
    }

    #[must_use]
    pub const fn same_file_name(self) -> bool {
        self.0 & Self::FILE_NAME != 0
    }

    #[must_use]
    pub const fn same_byte_len(self) -> bool {
        self.0 & Self::BYTE_LEN != 0
    }

    #[must_use]
    pub const fn same_capture_time(self) -> bool {
        self.0 & Self::CAPTURE_TIME != 0
    }

    #[must_use]
    pub const fn same_camera(self) -> bool {
        self.0 & Self::CAMERA != 0
    }

    const fn qualifies(self) -> bool {
        // A common camera filename alone is not enough. Allow either a stable
        // filename plus byte length, or three independent facts for a renamed
        // file. A metadata-only rewrite can still reach verification through
        // filename + capture time + camera, but it will not be merged unless a
        // future provider-specific payload identity confirms it.
        (self.same_file_name() && self.same_byte_len())
            || (self.same_file_name() && self.same_capture_time() && self.same_camera())
            || (self.same_byte_len() && self.same_capture_time() && self.same_camera())
    }
}

/// Why a discovered source cannot safely enter weak relocation matching.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum UnsupportedRelinkSource {
    /// Only user-owned originals can be relocated. Derived previews and
    /// proxies are rebuilt from their owner instead of relinked.
    UnsupportedRepresentationKind(RepresentationKind),
    /// There is no filename and no capture/camera pair to distinguish the
    /// source from arbitrary same-sized files.
    InsufficientWeakMetadata,
}

/// Result of weak, non-destructive relocation discovery.
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum SourceRelinkResolution {
    Unsupported(UnsupportedRelinkSource),
    NoLikelyCandidate,
    /// More than one candidate has enough weak evidence. Do not hash or merge
    /// automatically; a later UI can ask the photographer to choose.
    Ambiguous {
        candidates: Vec<RelinkCandidate>,
    },
    /// Exactly one candidate is plausible. Full-file identity verification is
    /// still required before any catalog lookup or attach.
    VerificationRequired(Box<PendingStrongRelink>),
}

/// A unique weak candidate awaiting strong identity verification.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct PendingStrongRelink {
    pub source: RelinkSource,
    pub candidate: RelinkCandidate,
    pub evidence: WeakRelinkEvidence,
}

impl PendingStrongRelink {
    /// Starts a strong relink verification after the photographer explicitly
    /// selected both the historical Library location and a replacement file.
    ///
    /// This deliberately carries no weak-evidence bits: filenames, EXIF, and
    /// byte length never authorize the attachment in this path. The complete
    /// file hash and the subsequent catalog owner check remain mandatory.
    #[must_use]
    pub fn explicitly_selected(source: RelinkSource, candidate: RelinkCandidate) -> Self {
        Self {
            source,
            candidate,
            evidence: WeakRelinkEvidence::from_bits(0),
        }
    }
}

/// A source whose complete file bytes were read consistently and hashed.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct VerifiedRelink {
    pub pending: PendingStrongRelink,
    pub identity: ContentIdentity,
}

/// Strong verification remains an outcome rather than changing a catalog
/// record. A changed source should simply be rediscovered with fresh metadata.
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum StrongRelinkVerification {
    SourceChanged {
        pending: PendingStrongRelink,
        observed_before: RepresentationFingerprint,
        observed_after: Option<RepresentationFingerprint>,
    },
    Verified(VerifiedRelink),
}

/// I/O failures while reading a source for strong identity verification.
#[derive(Debug, Error)]
pub enum RelinkVerificationError {
    #[error("cannot inspect relocation source {path}: {source}")]
    Metadata {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("relocation source is not a regular file: {path}")]
    NotRegularFile { path: PathBuf },
    #[error("cannot open relocation source {path}: {source}")]
    Open {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("cannot read relocation source {path}: {source}")]
    Read {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
}

/// Catalog read boundary required after a source has been strongly verified.
///
/// `Catalog` and its single-writer actor handle both implement this with the
/// same exact `relink_match` query. The lookup remains deliberately separate
/// from [`apply_confirmed_relink`], which still proves the target location is
/// absent and updates the import record atomically.
pub trait RelinkIdentityLookup {
    /// Looks up the exact, path-independent identity owner.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the identity cannot be queried.
    fn relink_match(&self, identity: &ContentIdentity)
    -> Result<Option<RelinkMatch>, CatalogError>;
}

impl RelinkIdentityLookup for Catalog {
    fn relink_match(
        &self,
        identity: &ContentIdentity,
    ) -> Result<Option<RelinkMatch>, CatalogError> {
        Self::relink_match(self, identity)
    }
}

impl RelinkIdentityLookup for CatalogHandle {
    fn relink_match(
        &self,
        identity: &ContentIdentity,
    ) -> Result<Option<RelinkMatch>, CatalogError> {
        Self::relink_match(self, identity)
    }
}

/// Result of asking a catalog whether a strongly verified source still belongs
/// to the weak candidate. This is deliberately read-only.
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum CatalogRelinkConfirmation {
    Ready(ConfirmedRelink),
    IdentityNotRecorded {
        verified: VerifiedRelink,
    },
    IdentityOwnedByAnotherRepresentation {
        verified: VerifiedRelink,
        actual: RelinkMatch,
    },
}

/// A verified source and catalog match that [`apply_confirmed_relink`] can
/// consume. Constructing this value alone never changes a path or photo.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ConfirmedRelink {
    pub verified: VerifiedRelink,
    pub matched: RelinkMatch,
}

/// Failure while applying an already confirmed relocation to the import
/// journal. A caller should rediscover and reverify after
/// [`Self::RegistrationNoLongerMatchesVerification`] rather than attaching a
/// path whose filesystem observations no longer match the bytes that were
/// hashed.
#[derive(Debug, Error)]
pub enum RelinkApplyError {
    #[error("the import registration no longer matches the source verified for relocation")]
    RegistrationNoLongerMatchesVerification,
    #[error("cannot attach confirmed relocation to the catalog: {0}")]
    Catalog(#[from] CatalogError),
}

/// Finds likely moved-source candidates without reading file contents or
/// mutating the catalog.
///
/// A candidate must agree on the original representation kind and at least two
/// independent observations under [`WeakRelinkEvidence::qualifies`]. More
/// than one plausible candidate is intentionally returned as ambiguous.
#[must_use]
pub fn discover_source_relink(
    source: RelinkSource,
    candidates: impl IntoIterator<Item = RelinkCandidate>,
) -> SourceRelinkResolution {
    if !supports_relink(source.kind) {
        return SourceRelinkResolution::Unsupported(
            UnsupportedRelinkSource::UnsupportedRepresentationKind(source.kind),
        );
    }
    if !has_weak_discriminator(&source.metadata, &source.path) {
        return SourceRelinkResolution::Unsupported(
            UnsupportedRelinkSource::InsufficientWeakMetadata,
        );
    }

    // A representation can legitimately have multiple historical or mounted
    // locations. Those locations must not turn one logical candidate into an
    // artificial ambiguity in the import UI.
    let mut represented = HashSet::new();
    let mut matching = candidates
        .into_iter()
        .filter(|candidate| represented.insert(candidate.representation_id))
        .filter_map(|candidate| {
            let evidence = weak_evidence(&source, &candidate);
            evidence.qualifies().then_some((candidate, evidence))
        })
        .collect::<Vec<_>>();

    match matching.len() {
        0 => SourceRelinkResolution::NoLikelyCandidate,
        1 => {
            let Some((candidate, evidence)) = matching.pop() else {
                return SourceRelinkResolution::NoLikelyCandidate;
            };
            SourceRelinkResolution::VerificationRequired(Box::new(PendingStrongRelink {
                source,
                candidate,
                evidence,
            }))
        }
        _ => SourceRelinkResolution::Ambiguous {
            candidates: matching
                .into_iter()
                .map(|(candidate, _)| candidate)
                .collect(),
        },
    }
}

/// Reads a unique relocation candidate completely and creates an exact
/// whole-file BLAKE3 identity only if its filesystem fingerprint was stable.
///
/// No catalog operation occurs here. This keeps a slow source hash off the
/// catalog writer and makes interrupted verification safe to retry.
///
/// # Errors
///
/// Returns [`RelinkVerificationError`] when the source cannot safely be
/// inspected, opened, or read.
pub fn verify_pending_relink(
    pending: PendingStrongRelink,
) -> Result<StrongRelinkVerification, RelinkVerificationError> {
    let observed_before = source_fingerprint(&pending.source.path)?;
    if observed_before != pending.source.fingerprint {
        return Ok(StrongRelinkVerification::SourceChanged {
            pending,
            observed_before,
            observed_after: None,
        });
    }

    let file =
        File::open(&pending.source.path).map_err(|source| RelinkVerificationError::Open {
            path: pending.source.path.clone(),
            source,
        })?;
    let mut reader = BufReader::with_capacity(1024 * 1024, file);
    let mut hasher = blake3::Hasher::new();
    let mut buffer = vec![0_u8; 1024 * 1024];
    loop {
        let read = reader
            .read(&mut buffer)
            .map_err(|source| RelinkVerificationError::Read {
                path: pending.source.path.clone(),
                source,
            })?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }

    let observed_after = source_fingerprint(&pending.source.path)?;
    if observed_after != observed_before {
        return Ok(StrongRelinkVerification::SourceChanged {
            pending,
            observed_before,
            observed_after: Some(observed_after),
        });
    }

    Ok(StrongRelinkVerification::Verified(VerifiedRelink {
        pending,
        identity: ContentIdentity::whole_file_blake3(*hasher.finalize().as_bytes()),
    }))
}

/// Confirms a strong identity against the candidate's representation without
/// attaching a path. Any mismatch remains explicit and non-destructive.
///
/// # Errors
///
/// Returns [`CatalogError`] when the identity lookup is unavailable or fails.
pub fn confirm_verified_relink(
    catalog: &impl RelinkIdentityLookup,
    verified: VerifiedRelink,
) -> Result<CatalogRelinkConfirmation, CatalogError> {
    match catalog.relink_match(&verified.identity)? {
        None => Ok(CatalogRelinkConfirmation::IdentityNotRecorded { verified }),
        Some(matched)
            if matched.representation_id == verified.pending.candidate.representation_id =>
        {
            Ok(CatalogRelinkConfirmation::Ready(ConfirmedRelink {
                verified,
                matched,
            }))
        }
        Some(actual) => Ok(
            CatalogRelinkConfirmation::IdentityOwnedByAnotherRepresentation { verified, actual },
        ),
    }
}

/// Applies a catalog-confirmed relocation through the normal import journal.
///
/// The caller must first persist the same `request` with
/// [`CatalogStore::record_import_discovered`]. This helper deliberately does
/// not do that implicitly: discovery is a user-visible scan event, whereas the
/// catalog then atomically attaches the verified new location and records its
/// `NeedsRevalidation` result.
///
/// It also rejects a registration that no longer names the exact path and
/// filesystem fingerprint that strong verification hashed. This prevents a
/// delayed UI action from attaching a changed or substituted file merely
/// because an old `ConfirmedRelink` object remains in memory.
///
/// # Errors
///
/// Returns [`RelinkApplyError`] if the request no longer represents the
/// verified source or the catalog's atomic identity proof fails.
pub fn apply_confirmed_relink(
    catalog: &mut (impl CatalogStore + ?Sized),
    session_id: ImportSessionId,
    request: &RegisterAsset,
    confirmed: &ConfirmedRelink,
) -> Result<RegisteredAsset, RelinkApplyError> {
    let source = &confirmed.verified.pending.source;
    let expected_location = native_location(&source.path);
    if request.kind != source.kind
        || request.location != expected_location
        || request.byte_len != source.fingerprint.byte_len
        || request.modified_at_ms != source.fingerprint.modified_at_ms
    {
        return Err(RelinkApplyError::RegistrationNoLongerMatchesVerification);
    }

    catalog
        .register_import_verified_relocation(
            session_id,
            request,
            confirmed.matched.representation_id,
            &confirmed.verified.identity,
        )
        .map_err(Into::into)
}

fn supports_relink(kind: RepresentationKind) -> bool {
    matches!(
        kind,
        RepresentationKind::OriginalRaw | RepresentationKind::OriginalRaster
    )
}

fn has_weak_discriminator(metadata: &WeakRelinkMetadata, path: &Path) -> bool {
    normalized_file_name(metadata, path).is_some()
        || (normalized_text(metadata.camera_key.as_deref()).is_some()
            && metadata.captured_at_unix_seconds.is_some())
}

fn weak_evidence(source: &RelinkSource, candidate: &RelinkCandidate) -> WeakRelinkEvidence {
    let same_kind = source.kind == candidate.kind && supports_relink(candidate.kind);
    let mut matched = 0;
    if same_kind
        && normalized_file_name(&source.metadata, &source.path)
            .zip(normalized_file_name(&candidate.metadata, Path::new("")))
            .is_some_and(|(source, candidate)| source == candidate)
    {
        matched |= WeakRelinkEvidence::FILE_NAME;
    }
    if same_kind && source.fingerprint.byte_len == candidate.fingerprint.byte_len {
        matched |= WeakRelinkEvidence::BYTE_LEN;
    }
    if same_kind
        && source
            .metadata
            .captured_at_unix_seconds
            .zip(candidate.metadata.captured_at_unix_seconds)
            .is_some_and(|(source, candidate)| source == candidate)
    {
        matched |= WeakRelinkEvidence::CAPTURE_TIME;
    }
    if same_kind
        && normalized_text(source.metadata.camera_key.as_deref())
            .zip(normalized_text(candidate.metadata.camera_key.as_deref()))
            .is_some_and(|(source, candidate)| source == candidate)
    {
        matched |= WeakRelinkEvidence::CAMERA;
    }
    WeakRelinkEvidence::from_bits(matched)
}

fn normalized_file_name(metadata: &WeakRelinkMetadata, fallback_path: &Path) -> Option<String> {
    let name = metadata.file_name.as_deref().or_else(|| {
        fallback_path
            .file_name()
            .and_then(|candidate| candidate.to_str())
    });
    normalized_text(name)
}

fn normalized_text(value: Option<&str>) -> Option<String> {
    let value = value?.trim();
    (!value.is_empty()).then(|| value.to_lowercase())
}

fn source_fingerprint(path: &Path) -> Result<RepresentationFingerprint, RelinkVerificationError> {
    let metadata =
        fs::symlink_metadata(path).map_err(|source| RelinkVerificationError::Metadata {
            path: path.to_path_buf(),
            source,
        })?;
    if !metadata.file_type().is_file() {
        return Err(RelinkVerificationError::NotRegularFile {
            path: path.to_path_buf(),
        });
    }
    Ok(RepresentationFingerprint {
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
    })
}

fn system_time_ms(time: SystemTime) -> Option<i64> {
    let duration = time.duration_since(UNIX_EPOCH).ok()?;
    i64::try_from(duration.as_millis()).ok()
}

#[cfg(test)]
mod tests {
    use std::fs;

    use super::*;
    use shadow_catalog::{Catalog, RecordRepresentationContentIdentity};
    use shadow_domain::{AssetLocation, EntityId, Platform};

    fn fingerprint(byte_len: u64) -> RepresentationFingerprint {
        RepresentationFingerprint {
            byte_len,
            modified_at_ms: Some(1_000),
        }
    }

    fn metadata(file_name: &str) -> WeakRelinkMetadata {
        WeakRelinkMetadata {
            file_name: Some(file_name.to_owned()),
            captured_at_unix_seconds: Some(1_700_000_000),
            camera_key: Some("Nikon Z 9".to_owned()),
        }
    }

    fn source(path: impl Into<PathBuf>, byte_len: u64) -> RelinkSource {
        RelinkSource {
            path: path.into(),
            kind: RepresentationKind::OriginalRaw,
            fingerprint: fingerprint(byte_len),
            metadata: metadata("DSC_0001.NEF"),
        }
    }

    fn candidate(file_name: &str, byte_len: u64) -> RelinkCandidate {
        RelinkCandidate {
            photo_id: PhotoId::new_v7(),
            representation_id: RepresentationId::new_v7(),
            kind: RepresentationKind::OriginalRaw,
            fingerprint: fingerprint(byte_len),
            metadata: metadata(file_name),
            location_label: format!("/former-library/{file_name}"),
        }
    }

    fn pending(path: impl Into<PathBuf>, byte_len: u64) -> PendingStrongRelink {
        let candidate = candidate("DSC_0001.NEF", byte_len);
        let source = source(path, byte_len);
        let SourceRelinkResolution::VerificationRequired(pending) =
            discover_source_relink(source, [candidate])
        else {
            panic!("fixture should have one weak candidate")
        };
        *pending
    }

    #[test]
    fn source_from_registration_preserves_the_normal_import_fingerprint() {
        let registration = RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::OtherUnix,
                b"/new/DSC_0001.NEF".to_vec(),
                "/new/DSC_0001.NEF",
            ),
            byte_len: 42,
            modified_at_ms: Some(123),
            now_ms: 456,
        };
        let source = RelinkSource::from_registration(
            "/new/DSC_0001.NEF",
            &registration,
            WeakRelinkMetadata::from_path(Path::new("/new/DSC_0001.NEF")),
        );

        assert_eq!(source.kind, registration.kind);
        assert_eq!(source.fingerprint.byte_len, registration.byte_len);
        assert_eq!(
            source.fingerprint.modified_at_ms,
            registration.modified_at_ms
        );
        assert_eq!(source.metadata.file_name.as_deref(), Some("DSC_0001.NEF"));
    }

    #[test]
    fn weak_discovery_requires_two_independent_facts() {
        let source = RelinkSource {
            path: PathBuf::from("/new/DSC_0001.NEF"),
            kind: RepresentationKind::OriginalRaw,
            fingerprint: fingerprint(10),
            metadata: WeakRelinkMetadata {
                file_name: Some("DSC_0001.NEF".to_owned()),
                captured_at_unix_seconds: None,
                camera_key: None,
            },
        };
        let candidate = RelinkCandidate {
            fingerprint: fingerprint(99),
            metadata: WeakRelinkMetadata {
                file_name: Some("DSC_0001.NEF".to_owned()),
                ..WeakRelinkMetadata::default()
            },
            ..candidate("irrelevant.NEF", 99)
        };

        assert_eq!(
            discover_source_relink(source, [candidate]),
            SourceRelinkResolution::NoLikelyCandidate
        );
    }

    #[test]
    fn weak_discovery_reports_ambiguous_candidates_without_hashing() {
        let source = source("/new/DSC_0001.NEF", 10);
        let first = candidate("DSC_0001.NEF", 10);
        let second = candidate("DSC_0001.NEF", 10);

        let SourceRelinkResolution::Ambiguous { candidates } =
            discover_source_relink(source, [first.clone(), second.clone()])
        else {
            panic!("two plausible candidates must remain ambiguous")
        };
        assert_eq!(candidates, vec![first, second]);
    }

    #[test]
    fn weak_discovery_deduplicates_multiple_locations_of_one_representation() {
        let source = source("/new/DSC_0001.NEF", 10);
        let first = candidate("DSC_0001.NEF", 10);
        let same_representation_elsewhere = RelinkCandidate {
            location_label: "/other-mounted-drive/DSC_0001.NEF".to_owned(),
            ..first.clone()
        };

        let SourceRelinkResolution::VerificationRequired(pending) =
            discover_source_relink(source, [first.clone(), same_representation_elsewhere])
        else {
            panic!("multiple locations of one representation are not ambiguous")
        };
        assert_eq!(pending.candidate, first);
    }

    #[test]
    fn weak_discovery_rejects_derived_representations() {
        let source = RelinkSource {
            kind: RepresentationKind::Proxy,
            ..source("/new/proxy.jpg", 10)
        };

        assert_eq!(
            discover_source_relink(source, std::iter::empty()),
            SourceRelinkResolution::Unsupported(
                UnsupportedRelinkSource::UnsupportedRepresentationKind(RepresentationKind::Proxy)
            )
        );
    }

    #[test]
    fn weak_discovery_accepts_a_renamed_file_only_with_three_matching_facts() {
        let source = RelinkSource {
            path: PathBuf::from("/new/renamed.nef"),
            kind: RepresentationKind::OriginalRaw,
            fingerprint: fingerprint(10),
            metadata: WeakRelinkMetadata {
                file_name: Some("renamed.nef".to_owned()),
                captured_at_unix_seconds: Some(1_700_000_000),
                camera_key: Some("nikon z 9".to_owned()),
            },
        };
        let candidate = candidate("DSC_0001.NEF", 10);

        let SourceRelinkResolution::VerificationRequired(pending) =
            discover_source_relink(source, [candidate])
        else {
            panic!("three matching facts should allow verification")
        };
        assert!(!pending.evidence.same_file_name());
        assert!(pending.evidence.same_byte_len());
        assert!(pending.evidence.same_capture_time());
        assert!(pending.evidence.same_camera());
    }

    #[test]
    fn verification_produces_a_stable_whole_file_identity() {
        let path = std::env::temp_dir().join(format!("shadow-relink-{}.nef", PhotoId::new_v7()));
        let bytes = b"original raw bytes";
        fs::write(&path, bytes).expect("write fixture");
        let current = source_fingerprint(&path).expect("stat fixture");
        let pending = pending(&path, current.byte_len);
        let pending = PendingStrongRelink {
            source: RelinkSource {
                fingerprint: current,
                ..pending.source
            },
            ..pending
        };

        let StrongRelinkVerification::Verified(verified) =
            verify_pending_relink(pending).expect("verify fixture")
        else {
            panic!("unchanged source must verify")
        };
        assert_eq!(
            verified.identity,
            ContentIdentity::whole_file_blake3(*blake3::hash(bytes).as_bytes())
        );
        fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn verification_returns_source_changed_instead_of_hashing_stale_registration() {
        let path =
            std::env::temp_dir().join(format!("shadow-relink-stale-{}.nef", PhotoId::new_v7()));
        fs::write(&path, b"fresh bytes").expect("write fixture");
        let pending = pending(&path, 1);

        let StrongRelinkVerification::SourceChanged {
            observed_before,
            observed_after,
            ..
        } = verify_pending_relink(pending).expect("inspect stale fixture")
        else {
            panic!("stale scanner fingerprint must not verify")
        };
        assert_eq!(observed_before.byte_len, b"fresh bytes".len() as u64);
        assert_eq!(observed_after, None);
        fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn catalog_confirmation_requires_the_exact_candidate_representation() {
        let path =
            std::env::temp_dir().join(format!("shadow-relink-catalog-{}.nef", PhotoId::new_v7()));
        let bytes = b"catalog identity bytes";
        fs::write(&path, bytes).expect("write fixture");
        let observed = source_fingerprint(&path).expect("stat fixture");
        let pending = pending(&path, observed.byte_len);
        let pending = PendingStrongRelink {
            source: RelinkSource {
                fingerprint: observed,
                ..pending.source
            },
            ..pending
        };
        let StrongRelinkVerification::Verified(verified) =
            verify_pending_relink(pending).expect("verify fixture")
        else {
            panic!("fixture should verify")
        };

        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let existing = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::OtherUnix,
                    b"/old/DSC_0001.NEF".to_vec(),
                    "/old/DSC_0001.NEF",
                ),
                byte_len: observed.byte_len,
                modified_at_ms: observed.modified_at_ms,
                now_ms: 1,
            })
            .expect("register old asset");
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: existing.representation_id,
                expected_source: observed,
                identity: verified.identity.clone(),
                observed_at_ms: 2,
            })
            .expect("record identity");

        let CatalogRelinkConfirmation::IdentityOwnedByAnotherRepresentation { actual, .. } =
            confirm_verified_relink(&catalog, verified)
                .expect("lookup exact identity without writing")
        else {
            panic!("a different candidate representation must not be merged")
        };
        assert_eq!(actual.representation_id, existing.representation_id);
        assert_eq!(catalog.stats().expect("stats").locations, 1);
        fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn catalog_confirmation_is_ready_only_for_the_exact_candidate() {
        let path =
            std::env::temp_dir().join(format!("shadow-relink-ready-{}.nef", PhotoId::new_v7()));
        let bytes = b"exact candidate bytes";
        fs::write(&path, bytes).expect("write fixture");
        let observed = source_fingerprint(&path).expect("stat fixture");
        let mut pending = pending(&path, observed.byte_len);
        pending.source.fingerprint = observed;

        let StrongRelinkVerification::Verified(mut verified) =
            verify_pending_relink(pending).expect("verify fixture")
        else {
            panic!("fixture should verify")
        };
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let existing = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::OtherUnix,
                    b"/old/DSC_0001.NEF".to_vec(),
                    "/old/DSC_0001.NEF",
                ),
                byte_len: observed.byte_len,
                modified_at_ms: observed.modified_at_ms,
                now_ms: 1,
            })
            .expect("register old asset");
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: existing.representation_id,
                expected_source: observed,
                identity: verified.identity.clone(),
                observed_at_ms: 2,
            })
            .expect("record identity");
        verified.pending.candidate.photo_id = existing.photo_id;
        verified.pending.candidate.representation_id = existing.representation_id;

        let CatalogRelinkConfirmation::Ready(confirmed) =
            confirm_verified_relink(&catalog, verified)
                .expect("lookup exact identity without writing")
        else {
            panic!("exact candidate should be ready for a later atomic attach")
        };
        assert_eq!(
            confirmed.matched.representation_id,
            existing.representation_id
        );
        assert_eq!(catalog.stats().expect("stats").locations, 1);
        fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn confirmed_relink_is_applied_only_for_the_exact_verified_import_request() {
        let path =
            std::env::temp_dir().join(format!("shadow-relink-apply-{}.nef", PhotoId::new_v7()));
        let bytes = b"verified relocation bytes";
        fs::write(&path, bytes).expect("write fixture");
        let observed = source_fingerprint(&path).expect("stat fixture");
        let mut pending = pending(&path, observed.byte_len);
        pending.source.fingerprint = observed;
        let StrongRelinkVerification::Verified(mut verified) =
            verify_pending_relink(pending).expect("verify fixture")
        else {
            panic!("fixture should verify")
        };

        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let existing = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::OtherUnix,
                    b"/former-drive/DSC_0001.NEF".to_vec(),
                    "/former-drive/DSC_0001.NEF",
                ),
                byte_len: observed.byte_len,
                modified_at_ms: observed.modified_at_ms,
                now_ms: 1,
            })
            .expect("register original");
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: existing.representation_id,
                expected_source: observed,
                identity: verified.identity.clone(),
                observed_at_ms: 2,
            })
            .expect("record identity");
        verified.pending.candidate.photo_id = existing.photo_id;
        verified.pending.candidate.representation_id = existing.representation_id;
        let CatalogRelinkConfirmation::Ready(confirmed) =
            confirm_verified_relink(&catalog, verified).expect("confirm exact identity")
        else {
            panic!("the recorded exact candidate must be ready")
        };

        let registration = RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&path),
            byte_len: observed.byte_len,
            modified_at_ms: observed.modified_at_ms,
            now_ms: 3,
        };
        let session = catalog
            .begin_import_session(&native_location(path.parent().expect("temp parent")), 3)
            .expect("begin import session");
        catalog
            .record_import_discovered(session, &registration)
            .expect("journal discovery");

        let attached = apply_confirmed_relink(&mut catalog, session, &registration, &confirmed)
            .expect("atomically attach verified source");
        assert_eq!(attached.photo_id, existing.photo_id);
        assert_eq!(attached.representation_id, existing.representation_id);
        assert_eq!(catalog.stats().expect("stats").locations, 2);

        let stale_request = RegisterAsset {
            byte_len: registration.byte_len + 1,
            ..registration.clone()
        };
        let error = apply_confirmed_relink(&mut catalog, session, &stale_request, &confirmed)
            .expect_err("changed registration must not attach a stale verification");
        assert!(matches!(
            error,
            RelinkApplyError::RegistrationNoLongerMatchesVerification
        ));
        assert_eq!(catalog.stats().expect("stats").locations, 2);
        fs::remove_file(path).expect("remove fixture");
    }
}
