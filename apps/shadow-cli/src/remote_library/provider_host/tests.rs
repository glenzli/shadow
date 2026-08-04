use shadow_core::DecodeInspector;

use super::RemoteLibraryServerPreviewInspector;

#[test]
fn public_only_runtime_does_not_claim_a_private_provider() {
    let inspector = RemoteLibraryServerPreviewInspector::public_only();
    assert_eq!(
        inspector.provider_id(),
        "shadow-remote-library-preview-router"
    );
    assert!(inspector.provider_version().contains(";public="));
    assert!(!inspector.provider_version().contains(";provider-host="));
}
