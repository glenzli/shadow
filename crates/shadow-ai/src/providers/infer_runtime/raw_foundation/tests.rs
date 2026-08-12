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
fn explicit_infer_raw_override_reports_the_exact_sdk_delta_without_transport() {
    let credential = tempfile::NamedTempFile::new().expect("credential fixture");
    let client =
        InferRuntimeClient::from_credential_file("http://127.0.0.1:65534", credential.path())
            .expect("official SDK adapter");
    let error = client
        .begin_raw_foundation(&request())
        .expect_err("RAW SDK surface is intentionally absent");
    let message = error.to_string();
    assert!(message.contains("infer-runtime-client@1.0.0"));
    assert!(message.contains("typed RAW ticket/SCM_RIGHTS/execution/cancellation"));
}
