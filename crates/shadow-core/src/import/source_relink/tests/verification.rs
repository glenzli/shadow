use std::fs;

use shadow_catalog::ContentIdentity;
use shadow_domain::{EntityId, PhotoId};

use super::{
    super::{
        PendingStrongRelink, RelinkSource, StrongRelinkVerification, source_fingerprint,
        verify_pending_relink,
    },
    candidate_scenarios::pending,
};

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
    let path = std::env::temp_dir().join(format!("shadow-relink-stale-{}.nef", PhotoId::new_v7()));
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
