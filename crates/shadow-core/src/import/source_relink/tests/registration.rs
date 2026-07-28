use std::path::Path;

use shadow_catalog::RegisterAsset;
use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use super::super::{RelinkSource, WeakRelinkMetadata};

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
