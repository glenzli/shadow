use shadow_core::DecodeInspector;

use super::LibraryServerPreviewInspector;

#[test]
fn public_only_runtime_does_not_claim_a_private_provider() {
    let inspector = LibraryServerPreviewInspector::public_only();
    assert_eq!(
        inspector.provider_id(),
        "shadow-remote-library-preview-router"
    );
    assert!(inspector.provider_version().contains(";public="));
    assert!(!inspector.provider_version().contains(";provider-host="));
}
