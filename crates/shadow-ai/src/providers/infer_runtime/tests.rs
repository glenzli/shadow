use std::{
    io::{Cursor, Read, Write},
    net::TcpListener,
    thread,
};

use base64::Engine as _;
use infer_runtime_client::{
    BoundingBox, EncodedLabelMap, FaceDetection, FaceParsingOntology, FaceParsingRegion,
    FaceParsingResponse, ImageGeometry, VisionProvenance as SdkVisionProvenance,
};
use sha2::Digest as _;

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

fn sdk_face_response() -> FaceDetectionResponse {
    FaceDetectionResponse {
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
    }
}

#[test]
fn sdk_face_fixture_is_admitted_only_after_shadow_geometry_checks() {
    let response = sdk_face_response();
    let admitted = admit_face_detection(response, "source-v1").expect("face evidence");
    assert_eq!(admitted.detections.len(), 1);
    assert_eq!(admitted.provenance.job_id, "vision-job");
}

#[test]
fn detector_clipped_landmarks_are_bounded_evidence_not_batch_failure() {
    let mut response = sdk_face_response();
    response.detections[0].landmarks.left_eye = Point { x: 32.0, y: 24.0 };
    let admitted = admit_face_detection(response, "source-v1").expect("closed detector extent");
    assert_eq!(admitted.detections.len(), 1);
    assert_eq!(admitted.detections[0].landmarks.left_eye.x, 32.0);
    for x in [-0.1, 32.1, f32::NAN, f32::INFINITY] {
        let mut response = sdk_face_response();
        response.detections[0].landmarks.left_eye.x = x;
        assert!(admit_face_detection(response, "source-v1").is_err());
    }
    let mut response = sdk_face_response();
    response.detections[0].bounding_box.width = 40.0;
    assert!(admit_face_detection(response, "source-v1").is_err());
}

#[test]
fn sdk_face_parsing_fixture_admits_only_verified_full_image_labels() {
    let labels = image::GrayImage::from_fn(4, 3, |x, _| image::Luma([(x % 4) as u8]));
    let mut encoded = Cursor::new(Vec::new());
    image::DynamicImage::ImageLuma8(labels.clone())
        .write_to(&mut encoded, image::ImageFormat::Png)
        .expect("encode label fixture");
    let bytes = encoded.into_inner();
    let regions = (0_u8..19)
        .map(|value| FaceParsingRegion {
            class_id: format!(
                "celebamask_hq_19:{}",
                FACE_PARSING_CLASS_IDS[usize::from(value)]
            ),
            label: FACE_PARSING_CLASS_IDS[usize::from(value)].replace('_', " "),
            label_value: value,
            pixel_count: if value < 4 { 3 } else { 0 },
            bounding_box: None,
        })
        .collect();
    let response = FaceParsingResponse {
        id: "face-parse-job".into(),
        object: "vision.face_parsing".into(),
        created_at: 1,
        status: "completed".into(),
        source_revision: "source-v2".into(),
        data_classification: BIOMETRIC_CLASSIFICATION.into(),
        image: ImageGeometry {
            width: 4,
            height: 3,
            orientation: EXPECTED_FACE_PARSING_ORIENTATION.into(),
        },
        face_box: BoundingBox {
            x: 0.0,
            y: 0.0,
            width: 4.0,
            height: 3.0,
        },
        label_map: EncodedLabelMap {
            content_type: "image/png".into(),
            encoding: "indexed_u8_png".into(),
            data_base64: STANDARD.encode(&bytes),
            sha256: format!("{:x}", Sha256::digest(&bytes)),
            width: 4,
            height: 3,
        },
        ontology: FaceParsingOntology {
            id: FACE_PARSING_ONTOLOGY.into(),
            revision: "20260813.1".into(),
            background_value: 0,
            class_count: 19,
        },
        regions,
        provenance: sdk_provenance(false),
    };

    let expected_box = FaceBoundingBox {
        x: 0.0,
        y: 0.0,
        width: 4.0,
        height: 3.0,
    };
    let parsed = admit_face_parsing(response.clone(), "source-v2", expected_box)
        .expect("face parsing evidence");
    assert_eq!(parsed.width, 4);
    assert_eq!(parsed.height, 3);
    assert_eq!(parsed.labels, labels.into_raw());

    let mut tampered = response;
    tampered.label_map.sha256 = "00".repeat(32);
    assert!(admit_face_parsing(tampered, "source-v2", expected_box).is_err());
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

#[test]
fn cancellation_drops_an_in_flight_request_without_waiting_for_its_deadline() {
    let credential = tempfile::NamedTempFile::new().expect("credential");
    let client =
        InferRuntimeClient::from_credential_file("http://127.0.0.1:9876", credential.path())
            .expect("client");
    let started = std::time::Instant::now();
    let result = client
        .block_on_cancellable(
            std::future::pending::<Result<(), InferRuntimeClientError>>(),
            &|| started.elapsed() >= std::time::Duration::from_millis(30),
        )
        .expect("cancel");
    assert!(result.is_none());
    assert!(started.elapsed() < std::time::Duration::from_secs(1));
}

#[test]
fn cancelled_face_parsing_never_stages_or_admits_an_image() {
    let credential = tempfile::NamedTempFile::new().unwrap();
    let client =
        InferRuntimeClient::from_credential_file("http://127.0.0.1:9876", credential.path())
            .unwrap();
    // Invalid bytes/geometry would fail if cancellation did not precede staging.
    let result = client
        .parse_face_cancellable(
            &[],
            "image/jpeg",
            "cancelled",
            FaceBoundingBox {
                x: 0.,
                y: 0.,
                width: 0.,
                height: 0.,
            },
            &|| true,
        )
        .unwrap();
    assert!(result.is_none());
}
