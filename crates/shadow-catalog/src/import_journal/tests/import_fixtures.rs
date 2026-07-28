use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::RegisterAsset;

pub(super) fn root() -> AssetLocation {
    AssetLocation::new(Platform::MacOs, b"/photos".to_vec(), "/photos")
}

pub(super) fn request() -> RegisterAsset {
    request_at("/photos/one.nef", 42, Some(100), 1_700_000_000_000)
}

pub(super) fn request_at(
    path: &str,
    byte_len: u64,
    modified_at_ms: Option<i64>,
    now_ms: i64,
) -> RegisterAsset {
    RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
        byte_len,
        modified_at_ms,
        now_ms,
    }
}
