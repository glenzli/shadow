use std::{
    io::{Read, Write},
    net::TcpListener,
    thread,
};

use infer_runtime_client::{
    BoundingBox, FaceDetection, ImageGeometry, VisionProvenance as SdkVisionProvenance,
};

use super::*;

const SDK_JOB_FIXTURE: &str = r#"{
  "id":"resp_example","app_id":"shadow","intent":"text.summarize",
  "consumer_core_contract":"infer-runtime.consumer-core@20260813.1",
  "capability_contract":"infer.responses@20260812.1","provider":"local-example",
  "deployment":"example-small","model_profile":"example-text-small",
  "model_build":"example-build-v1","physical_model":"example/model:small",
  "placement":"local","capability_level":"foundational","evaluation_status":"provisional",
  "resource_class":"light","state":"succeeded","policy":"local-first","priority":"normal",
  "constraints":{"policy":"local-first","priority":"normal","provider_access_class":"standard",
    "placement":"local_only","prefer":"local","offline_required":true,
    "capability_floor":"foundational","latency":"balanced","max_cost_usd":0.0,
    "fallback":"none","deadline_ms":null,"named_route":null},
  "routing":{"capability_floor":"foundational","named_route":null,"candidates":[{
    "deployment":"example-small","provider":"local-example","status":"eligible","rank":0,
    "reason_codes":[]}]},
  "attempts":[{"number":1,"provider":"local-example","deployment":"example-small",
    "outcome":"succeeded","trigger":"initial"}],"error":null
}"#;

#[test]
fn explicit_endpoint_is_only_a_validated_loopback_override() {
    let credential = tempfile::NamedTempFile::new().expect("credential fixture");
    assert!(
        InferRuntimeClient::from_credential_file("https://example.com", credential.path()).is_err()
    );
    assert!(
        InferRuntimeClient::from_credential_file("http://localhost:8787", credential.path())
            .is_err()
    );
    assert!(
        InferRuntimeClient::from_credential_file("http://127.0.0.1:9876/", credential.path())
            .is_err()
    );
    InferRuntimeClient::from_credential_file("http://127.0.0.1:9876", credential.path())
        .expect("path-free literal loopback override");
}

#[test]
fn sdk_job_fixture_preserves_shadow_app_and_dated_contract_without_daemon() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("fixture listener");
    let endpoint = format!("http://{}", listener.local_addr().expect("fixture address"));
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().expect("fixture connection");
        let mut request = Vec::new();
        let mut buffer = [0_u8; 4096];
        loop {
            let count = stream.read(&mut buffer).expect("read fixture request");
            if count == 0 {
                break;
            }
            request.extend_from_slice(&buffer[..count]);
            if request.windows(4).any(|window| window == b"\r\n\r\n") {
                break;
            }
        }
        let request = String::from_utf8(request).expect("HTTP request is text");
        assert!(request.starts_with("GET /infer/v1/jobs/resp_example HTTP/1.1\r\n"));
        let lower = request.to_ascii_lowercase();
        assert!(
            lower.contains("infer-consumer-contract: infer-runtime.consumer-core@20260813.1\r\n")
        );
        assert!(lower.contains("authorization: bearer "));
        let response = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            SDK_JOB_FIXTURE.len(),
            SDK_JOB_FIXTURE
        );
        stream
            .write_all(response.as_bytes())
            .expect("write fixture");
    });

    let mut credential = tempfile::NamedTempFile::new().expect("credential fixture");
    credential
        .write_all(b"shadow-managed-test-token-0000000000000000")
        .expect("write credential fixture");
    let client = InferRuntimeClient::from_credential_file(&endpoint, credential.path())
        .expect("official SDK client");
    let job = client.job("resp_example").expect("SDK Job fixture");
    assert_eq!(job.app_id, "shadow");
    assert_eq!(
        job.consumer_core_contract,
        "infer-runtime.consumer-core@20260813.1"
    );
    assert_eq!(
        job.capability_contract.as_deref(),
        Some("infer.responses@20260812.1")
    );
    assert_eq!(job.constraints["fallback"], "none");
    server.join().expect("fixture server");
}

#[test]
fn local_metadata_is_offline_and_has_no_fallback_or_candidate_vocabulary() {
    let metadata = local_metadata("background", Some("foundational"));
    assert_eq!(metadata["infer.priority"], "background");
    assert_eq!(metadata["infer.placement"], "local_only");
    assert_eq!(metadata["infer.offline_required"], "true");
    assert_eq!(metadata["infer.fallback"], "none");
    assert_eq!(metadata["infer.capability_floor"], "foundational");
    assert!(metadata.values().all(|value| !value.contains("candidate")));
}

#[test]
fn sdk_face_fixture_is_admitted_only_after_shadow_geometry_checks() {
    let response = FaceDetectionResponse {
        id: "face-job".into(),
        object: "vision.face_detection".into(),
        created_at: 1,
        status: "completed".into(),
        source_revision: "source-v1".into(),
        image: ImageGeometry {
            width: 32,
            height: 24,
            orientation: EXPECTED_FACE_ORIENTATION.into(),
        },
        detections: vec![FaceDetection {
            bounding_box: BoundingBox {
                x: 2.0,
                y: 3.0,
                width: 10.0,
                height: 12.0,
            },
            landmarks: FivePointLandmarks {
                right_eye: Point { x: 5.0, y: 7.0 },
                left_eye: Point { x: 9.0, y: 7.0 },
                nose_tip: Point { x: 7.0, y: 10.0 },
                right_mouth_corner: Point { x: 5.0, y: 13.0 },
                left_mouth_corner: Point { x: 9.0, y: 13.0 },
            },
            confidence: 0.95,
        }],
        provenance: sdk_provenance(false),
    };
    let admitted = admit_face_detection(response, "source-v1").expect("face evidence");
    assert_eq!(admitted.detections.len(), 1);
    assert_eq!(admitted.provenance.job_id, "vision-job");
}

fn sdk_provenance(tokenizer: bool) -> SdkVisionProvenance {
    let mut extra = BTreeMap::new();
    if tokenizer {
        extra.insert(
            "tokenizer".into(),
            serde_json::json!({
                "identity":"siglip-tokenizer-v1","artifact_sha256":"abc","max_length":64,
                "lowercase":true
            }),
        );
    }
    SdkVisionProvenance {
        job_id: "vision-job".into(),
        provider: "onnx-local".into(),
        deployment: "vision-local".into(),
        model_build: "vision-build".into(),
        artifact_sha256: "abc".into(),
        preprocessing_identity: "pre-v1".into(),
        postprocessing_identity: "post-v1".into(),
        runtime: "onnxruntime".into(),
        requested_execution_provider: "CPUExecutionProvider".into(),
        actual_execution_provider: "CPUExecutionProvider".into(),
        execution_provider_fallback_reason: None,
        precision: "float32".into(),
        extra,
    }
}
