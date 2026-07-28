use std::fs;

use shadow_catalog::{Catalog, RecordRepresentationContentIdentity, RegisterAsset};
use shadow_domain::{AssetLocation, EntityId, PhotoId, Platform, RepresentationKind};

use super::{
    super::{
        CatalogRelinkConfirmation, RelinkApplyError, StrongRelinkVerification,
        apply_confirmed_relink, confirm_verified_relink, source_fingerprint, verify_pending_relink,
    },
    candidate_scenarios::pending,
};
use crate::native_path::native_location;

#[test]
fn confirmed_relink_is_applied_only_for_the_exact_verified_import_request() {
    let path = std::env::temp_dir().join(format!("shadow-relink-apply-{}.nef", PhotoId::new_v7()));
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
