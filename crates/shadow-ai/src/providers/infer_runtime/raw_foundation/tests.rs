use super::*;

fn request() -> InferRawFoundationRequest {
    InferRawFoundationRequest::new(
        InferRawFoundationPriority::Background,
        Some(30_000),
        "shadow-source-v1",
        InferRawFoundationSource::new("a".repeat(64), 1024),
        InferRawFoundationStaging::new(
            8,
            6,
            "RGGB",
            [64; 4],
            [4095; 4],
            "b".repeat(64),
            InferRawFoundationDecoderIdentity::new("shadow.test-decoder", "1"),
        )
        .expect("valid staging"),
    )
}

#[test]
fn cache_identity_request_contract_remains_valid_before_runtime_selection() {
    let request = request();
    request.validate().expect("valid cache-miss request");
    let serialized = serde_json::to_value(request).expect("serialize request identity");
    assert_eq!(serialized["model"], RAW_FOUNDATION_INTENT);
    assert_eq!(serialized["staging"]["schema"], STAGING_SCHEMA);
    assert_eq!(
        serialized["source"]["pixel_contract_sha256"],
        SOURCE_PIXEL_CONTRACT_SHA256
    );
}

#[test]
fn invalid_staging_still_fails_before_any_runtime_call() {
    assert!(
        InferRawFoundationStaging::new(
            8,
            6,
            "RGBX",
            [64; 4],
            [4095; 4],
            "b".repeat(64),
            InferRawFoundationDecoderIdentity::new("shadow.test-decoder", "1"),
        )
        .is_err()
    );
}

#[test]
fn pinned_sdk_exposes_typed_raw_surface_before_transport() {
    infer_raw_foundation_sdk_status().expect("pinned SDK exposes the typed RAW surface");
}
