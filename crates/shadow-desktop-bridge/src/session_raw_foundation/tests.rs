use super::*;

#[test]
fn every_service_phase_has_one_stable_ffi_projection() {
    let cases = [
        (
            RawFoundationJobPhase::Queued,
            ffi::FfiRawFoundationJobPhase::Queued,
        ),
        (
            RawFoundationJobPhase::Planning,
            ffi::FfiRawFoundationJobPhase::Planning,
        ),
        (
            RawFoundationJobPhase::Running,
            ffi::FfiRawFoundationJobPhase::Running,
        ),
        (
            RawFoundationJobPhase::Ready,
            ffi::FfiRawFoundationJobPhase::Ready,
        ),
        (
            RawFoundationJobPhase::Unavailable,
            ffi::FfiRawFoundationJobPhase::Unavailable,
        ),
        (
            RawFoundationJobPhase::Cancelled,
            ffi::FfiRawFoundationJobPhase::Cancelled,
        ),
        (
            RawFoundationJobPhase::Failed,
            ffi::FfiRawFoundationJobPhase::Failed,
        ),
    ];
    for (source, expected) in cases {
        assert_eq!(ffi_phase(source), expected);
    }
}

#[test]
fn cache_disposition_encoding_preserves_reuse_and_publication() {
    assert_eq!(
        ffi_disposition(RawFoundationMaterializationDisposition::ReusedVerified),
        1
    );
    assert_eq!(
        ffi_disposition(RawFoundationMaterializationDisposition::Published),
        2
    );
    assert_eq!(
        ffi_disposition(RawFoundationMaterializationDisposition::ReusedConcurrent),
        3
    );
}
