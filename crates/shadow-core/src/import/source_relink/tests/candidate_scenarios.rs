use std::path::PathBuf;

use shadow_catalog::RepresentationFingerprint;
use shadow_domain::{EntityId, PhotoId, RepresentationId, RepresentationKind};

use super::super::{
    PendingStrongRelink, RelinkCandidate, RelinkSource, SourceRelinkResolution, WeakRelinkMetadata,
    discover_source_relink,
};

pub(super) fn fingerprint(byte_len: u64) -> RepresentationFingerprint {
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

pub(super) fn source(path: impl Into<PathBuf>, byte_len: u64) -> RelinkSource {
    RelinkSource {
        path: path.into(),
        kind: RepresentationKind::OriginalRaw,
        fingerprint: fingerprint(byte_len),
        metadata: metadata("DSC_0001.NEF"),
    }
}

pub(super) fn candidate(file_name: &str, byte_len: u64) -> RelinkCandidate {
    RelinkCandidate {
        photo_id: PhotoId::new_v7(),
        representation_id: RepresentationId::new_v7(),
        kind: RepresentationKind::OriginalRaw,
        fingerprint: fingerprint(byte_len),
        metadata: metadata(file_name),
        location_label: format!("/former-library/{file_name}"),
    }
}

pub(super) fn pending(path: impl Into<PathBuf>, byte_len: u64) -> PendingStrongRelink {
    let candidate = candidate("DSC_0001.NEF", byte_len);
    let source = source(path, byte_len);
    let SourceRelinkResolution::VerificationRequired(pending) =
        discover_source_relink(source, [candidate])
    else {
        panic!("fixture should have one weak candidate")
    };
    *pending
}
