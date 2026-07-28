use crate::{Catalog, RegisterAsset, RegisteredAsset};
use shadow_domain::{AssetLocation, ImportSessionId, Platform, RepresentationKind};

pub(super) fn register(catalog: &mut Catalog, path: &str) -> RegisteredAsset {
    register_kind(catalog, path, RepresentationKind::OriginalRaw)
}

pub(super) fn register_kind(
    catalog: &mut Catalog,
    path: &str,
    kind: RepresentationKind,
) -> RegisteredAsset {
    catalog
        .register_asset(&RegisterAsset {
            kind,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 100,
            modified_at_ms: Some(10),
            now_ms: 20,
        })
        .expect("register source")
}

pub(super) fn register_scan_entry(
    catalog: &mut Catalog,
    session_id: ImportSessionId,
    path: &str,
    now_ms: i64,
) -> RegisteredAsset {
    let request = RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
        byte_len: 100,
        modified_at_ms: Some(10),
        now_ms,
    };
    catalog
        .record_import_discovered(session_id, &request)
        .expect("record scan discovery");
    catalog
        .register_import_asset(session_id, &request)
        .expect("register scan entry")
}
