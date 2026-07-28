use std::fs;

use shadow_catalog::{Catalog, RecordRepresentationContentIdentity, RegisterAsset};
use shadow_domain::{AssetLocation, EntityId, PhotoId, Platform, RepresentationKind};

use super::{
    super::{
        CatalogRelinkConfirmation, PendingStrongRelink, RelinkSource, StrongRelinkVerification,
        confirm_verified_relink, source_fingerprint, verify_pending_relink,
    },
    candidate_scenarios::pending,
};

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
        confirm_verified_relink(&catalog, verified).expect("lookup exact identity without writing")
    else {
        panic!("a different candidate representation must not be merged")
    };
    assert_eq!(actual.representation_id, existing.representation_id);
    assert_eq!(catalog.stats().expect("stats").locations, 1);
    fs::remove_file(path).expect("remove fixture");
}

#[test]
fn catalog_confirmation_is_ready_only_for_the_exact_candidate() {
    let path = std::env::temp_dir().join(format!("shadow-relink-ready-{}.nef", PhotoId::new_v7()));
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
        confirm_verified_relink(&catalog, verified).expect("lookup exact identity without writing")
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
