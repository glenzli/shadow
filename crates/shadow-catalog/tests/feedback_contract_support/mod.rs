use shadow_ai::{PresentationContext, PresentedCandidate};
use shadow_catalog::{Catalog, RegisterAsset, RegisteredAsset, RegistrationStatus};
use shadow_domain::{AssetLocation, Platform, RepresentationKind};

pub fn register_source(catalog: &mut Catalog, index: u32) -> RegisteredAsset {
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                format!("/photos/feedback-contract-{index}.dng").into_bytes(),
                format!("/photos/feedback-contract-{index}.dng"),
            ),
            byte_len: 42,
            modified_at_ms: Some(i64::from(index)),
            now_ms: 1_700_000_000_000 + i64::from(index),
        })
        .expect("register feedback contract photo");
    assert_eq!(registered.status, RegistrationStatus::Inserted);
    registered
}

pub fn presentation(candidates: Vec<PresentedCandidate>) -> PresentationContext {
    PresentationContext {
        session_id: "feedback-contract-session".into(),
        group_id: None,
        candidates,
        active_model: None,
    }
}
