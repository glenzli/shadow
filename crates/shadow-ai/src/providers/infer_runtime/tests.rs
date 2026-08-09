use super::*;

fn provenance_json() -> serde_json::Value {
    serde_json::json!({
        "job_id": "vision-job",
        "provider": "onnx-local",
        "deployment": "onnx_yunet_2026may",
        "model_build": "yunet_2026may_onnx",
        "artifact_sha256": "artifact-sha256",
        "preprocessing_identity": "yunet-preprocess",
        "postprocessing_identity": "yunet-postprocess",
        "runtime": "onnxruntime-1.27.0",
        "requested_execution_provider": "coreml",
        "actual_execution_provider": "cpu",
        "execution_provider_fallback_reason": "unsupported graph",
        "precision": "fp32"
    })
}

fn landmarks_json() -> serde_json::Value {
    serde_json::json!({
        "right_eye": {"x": 2.0, "y": 2.0},
        "left_eye": {"x": 6.0, "y": 2.0},
        "nose_tip": {"x": 4.0, "y": 4.0},
        "right_mouth_corner": {"x": 2.5, "y": 6.0},
        "left_mouth_corner": {"x": 5.5, "y": 6.0}
    })
}

#[test]
fn client_rejects_non_loopback_and_path_bearing_urls() {
    let credential = InferRuntimeCredential::parse(&"a".repeat(64)).expect("credential");
    assert!(InferRuntimeClient::new("https://example.com", credential.clone()).is_err());
    assert!(InferRuntimeClient::new("http://localhost:8787", credential.clone()).is_err());
    assert!(InferRuntimeClient::new("http://127.0.0.1:8787/other", credential).is_err());
}

#[test]
fn credential_debug_output_is_redacted() {
    let secret = "a".repeat(64);
    let credential = InferRuntimeCredential::parse(&secret).expect("credential");
    let debug = format!("{credential:?}");
    assert!(debug.contains("redacted"));
    assert!(!debug.contains(&secret));
}

#[test]
fn detection_decoder_ignores_additive_fields_but_requires_exact_orientation() {
    let value = serde_json::json!({
        "id": "vision-job",
        "object": "vision.face_detection",
        "created_at": 1,
        "status": "completed",
        "source_revision": "shadow:source",
        "image": {
            "width": 8,
            "height": 8,
            "orientation": "input_pixels_no_exif_transform",
            "future_field": true
        },
        "detections": [{
            "bounding_box": {"x": 1.0, "y": 1.0, "width": 6.0, "height": 6.0},
            "landmarks": landmarks_json(),
            "confidence": 0.9,
            "future_field": "ignored"
        }],
        "provenance": provenance_json(),
        "future_field": "ignored"
    });
    let response: RawFaceDetectionResponse =
        serde_json::from_value(value.clone()).expect("decode additive response");
    assert!(response.validate("shadow:source").is_ok());

    let mut invalid = value;
    invalid["image"]["orientation"] = serde_json::json!("exif_applied");
    let response: RawFaceDetectionResponse =
        serde_json::from_value(invalid).expect("decode invalid response shape");
    assert!(response.validate("shadow:source").is_err());
}

#[test]
fn embedding_decoder_admits_only_normalized_sensitive_sface_vectors() {
    let mut values = vec![0.0_f32; crate::SFACE_EMBEDDING_DIMENSIONS];
    values[0] = 1.0;
    let response: RawFaceEmbeddingResponse = serde_json::from_value(serde_json::json!({
        "id": "vision-job",
        "object": "vision.face_embedding",
        "created_at": 1,
        "status": "completed",
        "source_revision": "shadow:source",
        "data_classification": "sensitive_biometric",
        "embedding": {
            "values": values,
            "dimensions": 128,
            "normalized": true,
            "distance_metric": "cosine",
            "space": "sface-build:artifact:postprocess"
        },
        "eligibility": {
            "eligible": true,
            "landmarks_in_image": true,
            "inter_eye_distance_pixels": 20.0,
            "alignment_rmse_pixels": 0.2
        },
        "provenance": provenance_json()
    }))
    .expect("decode embedding response");
    let embedded = response
        .validate("shadow:source")
        .expect("validate embedding");
    assert!(format!("{:?}", embedded.embedding).contains("redacted"));
}
