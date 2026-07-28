//! A catalog-backed edit session with one registered RAW source.

use std::path::PathBuf;

use shadow_catalog::RegisterAsset;
use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationId, RepresentationKind};

use crate::{DesktopSession, open_desktop_session};

pub(in crate::tests) fn test_edit_session() -> (PathBuf, Box<DesktopSession>, String, String) {
    let root = std::env::temp_dir().join(format!(
        "shadow-desktop-edit-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create edit fixture");
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open edit session");
    let source_path = root
        .join("input.dng")
        .to_str()
        .expect("source path")
        .to_owned();
    let registered = session
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                source_path.as_bytes().to_vec(),
                source_path.clone(),
            ),
            byte_len: 4_096,
            modified_at_ms: Some(123),
            now_ms: 100,
        })
        .expect("register edit source");
    (root, session, registered.photo_id.to_string(), source_path)
}
