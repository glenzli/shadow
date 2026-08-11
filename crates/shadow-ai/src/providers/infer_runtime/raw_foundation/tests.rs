use std::{
    fs::{self, File, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    thread,
    time::{Duration, SystemTime},
};

use serde_json::{Value, json};

use super::*;

fn sha(character: char) -> String {
    std::iter::repeat_n(character, 64).collect()
}

fn request() -> InferRawFoundationRequest {
    InferRawFoundationRequest::new(
        InferRawFoundationPriority::Background,
        Some(30_000),
        "shadow:raw:test:v1",
        InferRawFoundationSource {
            sha256: sha('1'),
            size_bytes: 1_234,
            pixel_contract_sha256: SOURCE_PIXEL_CONTRACT_SHA256.into(),
        },
        InferRawFoundationStaging::new(
            8,
            6,
            "RGGB",
            [64; 4],
            [4_095; 4],
            sha('2'),
            InferRawFoundationDecoderIdentity::new("shadow.test-decoder", "1"),
        )
        .expect("valid staging"),
    )
}

fn provenance_json() -> Value {
    json!({
        "provider": EXPECTED_PROVIDER,
        "deployment": EXPECTED_DEPLOYMENT,
        "model_profile": EXPECTED_MODEL_PROFILE,
        "model_build": EXPECTED_MODEL_BUILD,
        "physical_model": EXPECTED_PHYSICAL_MODEL,
        "exact_revision": EXPECTED_EXACT_REVISION,
        "graph_sha256": EXPECTED_GRAPH_SHA256,
        "implementation_revision": EXPECTED_IMPLEMENTATION_REVISION,
        "cache_identity": EXPECTED_CACHE_IDENTITY,
        "execution_provider": EXPECTED_EXECUTION_PROVIDER,
        "runtime_version": EXPECTED_RUNTIME_VERSION,
        "precision": EXPECTED_PRECISION,
        "future_field": true
    })
}

fn artifact_json(file_bytes: u64) -> Value {
    json!({
        "cache_key_sha256": sha('3'),
        "artifact_identity_sha256": sha('4'),
        "artifact_file_sha256": sha('5'),
        "artifact_file_bytes": file_bytes,
        "sequence_sha256": sha('6'),
        "payload_sha256": sha('7'),
        "output_width": 8,
        "output_height": 6,
        "tile_inferences": 2,
        "maximum_accumulator_rows": 6,
        "explicit_full_output_buffers": 0,
        "implementation_revision": EXPECTED_IMPLEMENTATION_REVISION,
        "cache_identity": EXPECTED_CACHE_IDENTITY,
        "future_field": "ignored"
    })
}

#[test]
fn request_serializes_only_the_frozen_descriptor_and_rejects_invalid_bounds() {
    let request = request();
    request.validate().expect("valid request");
    let value = serde_json::to_value(&request).expect("serialize request");
    assert_eq!(value["model"], RAW_FOUNDATION_INTENT);
    assert_eq!(value["staging"]["sample_bytes"], 96);
    assert!(value.get("app_id").is_none());
    assert!(value.get("path").is_none());
    assert!(value["staging"].get("path").is_none());

    let mut invalid = request.clone();
    invalid.deadline_ms = Some(0);
    assert!(invalid.validate().is_err());

    let mut invalid = request;
    invalid.staging.white_levels[2] = 4_094;
    assert!(invalid.validate().is_err());
}

#[test]
fn response_ignores_additive_fields_but_requires_the_exact_build_pair() {
    let value = json!({
        "id": "raw_job",
        "object": "raw.foundation",
        "status": "completed",
        "source_revision": "shadow:raw:test:v1",
        "artifact": artifact_json(128),
        "provenance": provenance_json(),
        "future_field": {"ignored": true}
    });
    let response: RawFoundationResponse =
        serde_json::from_value(value.clone()).expect("decode additive response");
    assert!(
        response
            .validate("raw_job", "shadow:raw:test:v1", 8, 6)
            .is_ok()
    );

    let mut invalid = value.clone();
    invalid["provenance"]["runtime_version"] = json!("onnxruntime-1.28.0");
    let response: RawFoundationResponse =
        serde_json::from_value(invalid).expect("decode invalid build response");
    assert!(
        response
            .validate("raw_job", "shadow:raw:test:v1", 8, 6)
            .is_err()
    );

    let mut invalid = value;
    invalid["provenance"]["graph_sha256"] = json!(sha('f'));
    let response: RawFoundationResponse =
        serde_json::from_value(invalid).expect("decode graph drift response");
    assert!(
        response
            .validate("raw_job", "shadow:raw:test:v1", 8, 6)
            .is_err()
    );
}

#[test]
fn lease_and_job_debug_output_redacts_capabilities_and_socket_path() {
    let endpoint = DiscoveryEndpoint::explicit(
        super::super::validate_loopback_base_url("http://127.0.0.1:8787").expect("valid endpoint"),
    );
    let grant = InferRawFoundationLeaseGrant {
        job: InferRawFoundationJob {
            id: "raw_job".into(),
            endpoint,
        },
        ticket_id: "ticket_secret".into(),
        expires_at_unix_ms: 123,
        daemon_generation: "gen_test".into(),
        socket_path: PathBuf::from("/private/secret/lease.sock"),
        source_revision: "source".into(),
        width: 8,
        height: 6,
        execution_timeout: Duration::from_secs(1),
    };
    let debug = format!("{grant:?}");
    assert!(debug.contains("redacted"));
    assert!(!debug.contains("ticket_secret"));
    assert!(!debug.contains("/private/secret"));
    assert!(!format!("{:?}", grant.job()).contains("http://"));
}

#[test]
fn execute_request_contains_only_the_job_and_one_shot_lease() {
    let request = RawExecuteRequest {
        job_id: "raw_job",
        lease_id: "lease_secret",
    };
    assert_eq!(
        serde_json::to_value(request).expect("execute request"),
        json!({"job_id":"raw_job", "lease_id":"lease_secret"})
    );
}

#[cfg(unix)]
#[test]
fn unix_registration_sends_exact_line_and_two_handles() {
    use std::os::unix::{fs::OpenOptionsExt, net::UnixListener};

    let directory = private_test_directory("lease");
    let socket_path = directory.join("lease.sock");
    let listener = UnixListener::bind(&socket_path).expect("bind socket");
    fs::set_permissions(&socket_path, private_permissions(0o600)).expect("socket mode");
    let expires_at = 42_000;
    let server = spawn_registration_server(listener, expires_at);

    let input_path = directory.join("input.u16le");
    let output_path = directory.join("output.shadowrawf");
    let mut input_writer = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&input_path)
        .expect("create input");
    input_writer.write_all(&[0_u8; 32]).expect("write input");
    drop(input_writer);
    let input = File::open(&input_path).expect("read-only input");
    let output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&output_path)
        .expect("exclusive output");
    let endpoint = DiscoveryEndpoint::explicit(
        super::super::validate_loopback_base_url("http://127.0.0.1:8787").expect("valid endpoint"),
    );
    let grant = InferRawFoundationLeaseGrant {
        job: InferRawFoundationJob {
            id: "raw_job".into(),
            endpoint,
        },
        ticket_id: "ticket_secret".into(),
        expires_at_unix_ms: expires_at,
        daemon_generation: "gen_test".into(),
        socket_path: socket_path.clone(),
        source_revision: "source".into(),
        width: 4,
        height: 4,
        execution_timeout: Duration::from_secs(1),
    };
    let lease = artifact_lease::register(grant, &input, &output).expect("register handles");
    assert_eq!(lease.job().id(), "raw_job");
    assert_eq!(lease.expires_at_unix_ms(), expires_at);
    assert!(!format!("{lease:?}").contains("lease_secret"));
    server.join().expect("registration server");
    fs::remove_file(socket_path).expect("remove socket");
    remove_test_directory(&directory);
}

#[cfg(unix)]
fn spawn_registration_server(
    listener: std::os::unix::net::UnixListener,
    expires_at: u64,
) -> thread::JoinHandle<()> {
    use std::{
        io::{IoSliceMut, Read as _},
        os::fd::AsRawFd,
    };

    use nix::{
        cmsg_space,
        sys::socket::{ControlMessageOwned, MsgFlags, recvmsg},
        unistd::close,
    };

    thread::spawn(move || {
        let (mut stream, _) = listener.accept().expect("accept");
        let mut frame = [0_u8; 4_096];
        let (received, descriptors) = {
            let mut slices = [IoSliceMut::new(&mut frame)];
            let mut control = cmsg_space!([std::os::fd::RawFd; 2]);
            let message = recvmsg::<()>(
                stream.as_raw_fd(),
                &mut slices,
                Some(&mut control),
                MsgFlags::empty(),
            )
            .expect("receive registration");
            let descriptors = message
                .cmsgs()
                .expect("control messages")
                .flat_map(|message| match message {
                    ControlMessageOwned::ScmRights(descriptors) => descriptors,
                    _ => Vec::new(),
                })
                .collect::<Vec<_>>();
            (message.bytes, descriptors)
        };
        let mut tail = Vec::new();
        stream.read_to_end(&mut tail).expect("registration EOF");
        let mut bytes = frame[..received].to_vec();
        bytes.extend_from_slice(&tail);
        assert_eq!(descriptors.len(), 2);
        for descriptor in descriptors {
            close(descriptor).expect("close received descriptor");
        }
        assert_eq!(
            serde_json::from_slice::<Value>(bytes.strip_suffix(b"\n").expect("one line"))
                .expect("registration JSON"),
            json!({
                "schema":"infer.artifact-lease.register",
                "schema_version":"20260811.1",
                "operation":"register",
                "ticket_id":"ticket_secret"
            })
        );
        stream
            .write_all(
                format!(
                    "{{\"schema\":\"infer.artifact-lease.registered\",\"schema_version\":\"20260811.1\",\"lease_id\":\"lease_secret\",\"expires_at_unix_ms\":{expires_at}}}\n"
                )
                .as_bytes(),
            )
            .expect("registration response");
    })
}

fn private_test_directory(label: &str) -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .expect("time after epoch")
        .as_nanos();
    #[cfg(unix)]
    let root = Path::new("/private/tmp");
    #[cfg(not(unix))]
    let root = std::env::temp_dir();
    let path = root.join(format!("sri-{label}-{}-{nanos}", std::process::id()));
    fs::create_dir(&path).expect("create private test directory");
    fs::set_permissions(&path, private_permissions(0o700)).expect("private directory mode");
    path
}

#[cfg(unix)]
fn private_permissions(mode: u32) -> fs::Permissions {
    use std::os::unix::fs::PermissionsExt;
    fs::Permissions::from_mode(mode)
}

#[cfg(not(unix))]
fn private_permissions(_mode: u32) -> fs::Permissions {
    fs::metadata(std::env::temp_dir())
        .expect("temp metadata")
        .permissions()
}

fn remove_test_directory(path: &Path) {
    fs::remove_dir_all(path).expect("remove test directory");
}
