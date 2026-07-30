use std::path::PathBuf;

#[cfg(unix)]
use std::{
    fs,
    path::Path,
    sync::atomic::{AtomicU64, Ordering},
};

use super::*;

#[cfg(unix)]
static REAL_FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[test]
fn request_binds_one_exact_raw_file_to_the_photo_generation() {
    let invocation = RawFoundationInvocation {
        request_id: "raw-foundation-request-7".into(),
        generation: 7,
        photo_id: "018f3ec1-6219-7df2-a52d-f744c4f88533".into(),
        input_raw: PathBuf::from("/source.CR2"),
    };
    let photo_id = PhotoId::from_str(&invocation.photo_id).expect("photo id");
    let estimate = ResourceEstimate {
        peak_system_ram_bytes: 2 * GIBIBYTE,
        peak_device_memory_bytes: 0,
        cpu_threads: 4,
        scratch_disk_bytes: 2 * GIBIBYTE,
        upload_bytes: 0,
        estimated_duration_ms: Some(180_000),
    };

    let request = foundation_request(&invocation, photo_id, "a".repeat(64), 17, estimate);

    assert_eq!(request.request_id, invocation.request_id);
    assert_eq!(request.generation, 7);
    assert_eq!(request.task, AiTaskKind::MaterializeRawFoundation);
    assert_eq!(request.parameters, AiTaskParameters::RawFoundation);
    assert_eq!(request.inputs.len(), 1);
    assert_eq!(request.inputs[0].role, InputRole::RawFile);
    assert_eq!(request.inputs[0].content_hash, "a".repeat(64));
    assert_eq!(request.inputs[0].byte_len, 17);
    assert_eq!(request.inputs[0].media_type, "image/x-raw");
}

#[cfg(unix)]
#[test]
fn real_public_runtime_publishes_then_reuses_the_verified_foundation_when_supplied() {
    use std::os::unix::fs::PermissionsExt;

    let variable_names = [
        "SHADOW_TEST_RAWNIND_PYTHON",
        "SHADOW_TEST_RAWNIND_SIDECAR",
        "SHADOW_TEST_RAWNIND_PACKAGE",
        "SHADOW_TEST_RAWNIND_GRAPH",
        "SHADOW_TEST_RAWNIND_RAW",
    ];
    let Some(values) = variable_names
        .map(std::env::var_os)
        .into_iter()
        .collect::<Option<Vec<_>>>()
    else {
        eprintln!("skipping real desktop RawNIND runtime acceptance; paths were not supplied");
        return;
    };
    let python = PathBuf::from(&values[0]);
    let sidecar = PathBuf::from(&values[1]);
    let package = PathBuf::from(&values[2]);
    let graph = PathBuf::from(&values[3]);
    let raw = PathBuf::from(&values[4]);
    let sequence = REAL_FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let root = std::env::temp_dir().join(format!(
        "shadow-desktop-rawnind-runtime-{}-{sequence}",
        std::process::id()
    ));
    fs::create_dir_all(&root).expect("real runtime root");
    let launcher = root.join("provider-launcher.sh");
    fs::write(
        &launcher,
        format!(
            "#!/bin/sh\nexec {} {} \"$@\"\n",
            shell_single_quote(&python),
            shell_single_quote(&sidecar),
        ),
    )
    .expect("provider launcher");
    let mut permissions = fs::metadata(&launcher).unwrap().permissions();
    permissions.set_mode(0o700);
    fs::set_permissions(&launcher, permissions).expect("launcher permission");
    let manifest_path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../apps/desktop/providers/rawnind-foundation/model-manifest.json");
    let runtime_paths = RawFoundationRuntimePaths {
        provider_executable: launcher,
        model_package: package,
        model_graph: graph,
        manifest_path,
        foundation_store_root: root.join("cache"),
        raw_frame_staging_root: root.join("raw-frame-staging"),
    };
    let runtime =
        RawFoundationRuntime::open(runtime_paths.clone()).expect("desktop RAW foundation runtime");
    let availability = runtime
        .probe(&CancellationToken::default())
        .expect("real model availability");
    assert_eq!(
        availability.model_id,
        shadow_ai::RAWNIND_FOUNDATION_MODEL_ID
    );

    let first = runtime
        .materialize(
            &RawFoundationInvocation {
                request_id: "desktop-real-first".into(),
                generation: 1,
                photo_id: "018f3ec1-6219-7df2-a52d-f744c4f88533".into(),
                input_raw: raw.clone(),
            },
            &CancellationToken::default(),
            &DiscardProgress,
        )
        .expect("first desktop materialization");
    let RawFoundationRuntimeOutcome::Ready(first) = first else {
        panic!("first desktop materialization was not ready");
    };
    assert_eq!(
        first.disposition,
        RawFoundationMaterializationDisposition::Published
    );
    assert_eq!(first.descriptor.raster_extent().width, 3908);
    assert_eq!(first.descriptor.raster_extent().height, 2600);
    assert_eq!(first.source_path, raw);
    assert_eq!(
        first.source,
        fingerprint_source(&first.source_path).expect("ready source fingerprint")
    );

    let second = runtime
        .materialize(
            &RawFoundationInvocation {
                request_id: "desktop-real-second".into(),
                generation: 2,
                photo_id: "018f3ec1-6219-7df2-a52d-f744c4f88533".into(),
                input_raw: raw.clone(),
            },
            &CancellationToken::default(),
            &DiscardProgress,
        )
        .expect("cached desktop materialization");
    let RawFoundationRuntimeOutcome::Ready(second) = second else {
        panic!("cached desktop materialization was not ready");
    };
    assert_eq!(
        second.disposition,
        RawFoundationMaterializationDisposition::ReusedVerified
    );
    assert_eq!(second.path, first.path);
    assert_eq!(second.descriptor, first.descriptor);

    let reopened_runtime =
        RawFoundationRuntime::open(runtime_paths).expect("reopen desktop RAW foundation runtime");
    let reopened = reopened_runtime
        .resolve_cached(&raw, &CancellationToken::default())
        .expect("resolve cached foundation after restart")
        .expect("cached foundation remains available");
    assert_eq!(
        reopened.disposition,
        RawFoundationMaterializationDisposition::ReusedVerified
    );
    assert_eq!(reopened.path, first.path);
    assert_eq!(reopened.descriptor, first.descriptor);
    assert_eq!(reopened.source_path, raw);
    assert_eq!(reopened.source, first.source);
    fs::remove_dir_all(root).expect("remove real runtime fixture");
}

#[derive(Debug)]
struct DiscardProgress;

impl RuntimeProgressSink for DiscardProgress {
    fn publish(&self, _progress: shadow_ai::RuntimeProgress) {}
}

#[cfg(unix)]
fn shell_single_quote(path: &Path) -> String {
    format!("'{}'", path.to_string_lossy().replace('\'', "'\"'\"'"))
}
