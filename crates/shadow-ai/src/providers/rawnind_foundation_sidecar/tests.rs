use std::{
    fs,
    path::{Path, PathBuf},
    str::FromStr,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    thread,
    time::Duration,
};

use shadow_cache::{FoundationArtifactStore, FoundationArtifactStripe, verify_foundation_artifact};
use shadow_domain::{PhotoId, Platform};

use super::*;
use crate::{
    AI_JOB_REQUEST_CONTRACT_VERSION, ArtifactReference, ExecutionBackend, ExecutionLease,
    FallbackDisclosure, HardwareProfile, LocalExecutionAdmission, LocalExecutionBinding,
    LocalModelAvailability, ModelArtifactSet, ModelAvailability, ObservationTarget,
    OnBatteryPolicy, PrivacyClass, RawFoundationMaterializationDisposition,
    RawFoundationMaterializationOutcome, ResourceEstimate, ResourcePolicy, RuntimeTerminalOutcome,
    TaskPriority, admit_local_execution, materialize_rawnind_foundation,
};

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[test]
fn checked_in_manifest_has_the_canonical_public_inventory() {
    let json: serde_json::Value =
        serde_json::from_slice(CHECKED_IN_MANIFEST).expect("manifest JSON");
    let artifact_set: ModelArtifactSet =
        serde_json::from_value(json["artifact_set"].clone()).expect("artifact set");
    let computed = artifact_set
        .computed_inventory_blake3()
        .expect("canonical inventory");
    println!("RAWNIND_ARTIFACT_SET={computed}");
    assert_eq!(computed, RAWNIND_FOUNDATION_ARTIFACT_SET_BLAKE3);
    let manifest: ModelManifest =
        serde_json::from_slice(CHECKED_IN_MANIFEST).expect("validated manifest");
    assert_eq!(manifest.model_id, RAWNIND_FOUNDATION_MODEL_ID);
}

#[test]
fn receipts_pin_runtime_model_and_complete_foundation_identity() {
    let model = format!(
        "{RAWNIND_FOUNDATION_MODEL_RECEIPT_PREFIX} \
         package_sha256={RAWNIND_FOUNDATION_PACKAGE_SHA256} \
         graph_sha256={RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256} \
         runtime_version=1.24.4\n"
    );
    assert_eq!(
        parse_model_receipt(model.as_bytes()).as_deref(),
        Ok("1.24.4")
    );
    let plan = parse_plan_receipt(
        format!(
            "{RAWNIND_FOUNDATION_PLAN_RECEIPT_PREFIX} cache_key_sha256={} \
             source_sha256={} source_size_bytes=17 source_pixel_contract_sha256={} \
             width=6000 height=4000 runtime_version=1.24.4\n",
            "c".repeat(64),
            "a".repeat(64),
            RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256,
        )
        .as_bytes(),
    )
    .expect("valid foundation plan");
    assert_eq!(plan.raster_extent(), RasterExtent::new(6000, 4000).unwrap());

    let receipt = parse_foundation_receipt(
        format!(
            "{RAWNIND_FOUNDATION_RECEIPT_PREFIX} cache_key_sha256={} \
             artifact_identity_sha256={} file_sha256={} width=6000 height=4000 \
             runtime_version=1.24.4\n",
            "c".repeat(64),
            "d".repeat(64),
            "f".repeat(64)
        )
        .as_bytes(),
    )
    .expect("valid foundation receipt");
    assert_eq!(receipt.width, 6000);
    assert_eq!(receipt.height, 4000);
}

#[test]
fn descriptor_requires_receipt_source_model_and_file_agreement() {
    let verification = fixture_verification(PathBuf::from("fixture.shadowrawf"));
    let receipt = fixture_receipt();
    let descriptor = descriptor_from_verification(
        verification.clone(),
        &"a".repeat(64),
        17,
        RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256,
        "1.24.4",
        &fixture_plan(),
        &receipt,
    )
    .expect("matching verified descriptor");
    assert_eq!(descriptor.raster_extent().width, 6000);
    assert_eq!(
        descriptor.provenance().cache_key_sha256(),
        verification.cache_key_sha256
    );

    let mut substituted = receipt;
    substituted.file_sha256 = "0".repeat(64);
    assert!(
        descriptor_from_verification(
            verification,
            &"a".repeat(64),
            17,
            RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256,
            "1.24.4",
            &fixture_plan(),
            &substituted,
        )
        .is_err()
    );
}

#[cfg(unix)]
#[test]
fn verified_fake_sidecar_runs_only_the_explicit_foundation_task_once() {
    let fixture = Fixture::new("success");
    let installation = fixture.installation();
    let execution = Fixture::execution(&installation, "a".repeat(64), 17);
    let execution_plan = execution.plan_identity().clone();
    let foundation_plan = fixture.foundation_plan(&installation);
    let provider = RawNindFoundationProvider::new_with_verifier(
        &installation,
        execution_plan,
        foundation_plan,
        fixture.input_raw.clone(),
        fixture.output_foundation.clone(),
        Arc::new(FixtureOutputVerifier {
            verification: fixture_verification(fixture.output_foundation.clone()),
        }),
    )
    .expect("configured provider");

    let receipt = ExecutionLease::issue("rawnind-lease-1".into(), execution)
        .expect("execution lease")
        .execute(&provider, &CancellationToken::default());
    let RuntimeTerminalOutcome::Succeeded { output } = receipt.outcome else {
        panic!("fake provider must produce a verified RAW foundation");
    };
    assert_eq!(
        output.payload().source().source_file_sha256(),
        "a".repeat(64)
    );
    assert_eq!(
        output.payload().provenance().model_graph_sha256(),
        RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256
    );
    assert!(fixture.output_foundation.exists());
}

#[cfg(unix)]
#[test]
fn cancellation_kills_the_sidecar_and_discards_its_unpublished_partial() {
    let fixture = Fixture::new("cancel");
    let installation = fixture.installation();
    let execution = Fixture::execution(&installation, "a".repeat(64), 17);
    let execution_plan = execution.plan_identity().clone();
    let foundation_plan = fixture.foundation_plan(&installation);
    let provider = RawNindFoundationProvider::new_with_verifier(
        &installation,
        execution_plan,
        foundation_plan,
        fixture.input_raw.clone(),
        fixture.output_foundation.clone(),
        Arc::new(FixtureOutputVerifier {
            verification: fixture_verification(fixture.output_foundation.clone()),
        }),
    )
    .expect("configured provider");
    let cancellation = CancellationToken::default();
    let cancellation_for_thread = cancellation.clone();
    let canceller = thread::spawn(move || {
        thread::sleep(Duration::from_millis(40));
        cancellation_for_thread.cancel();
    });

    let receipt = ExecutionLease::issue("rawnind-lease-cancel".into(), execution)
        .expect("execution lease")
        .execute(&provider, &cancellation);
    canceller.join().expect("canceller");

    assert!(matches!(receipt.outcome, RuntimeTerminalOutcome::Cancelled));
    assert!(!fixture.output_foundation.exists());
}

#[cfg(unix)]
#[test]
fn malformed_success_receipt_discards_the_unpublished_partial() {
    let fixture = Fixture::new("invalid");
    let installation = fixture.installation();
    let execution = Fixture::execution(&installation, "a".repeat(64), 17);
    let execution_plan = execution.plan_identity().clone();
    let foundation_plan = fixture.foundation_plan(&installation);
    let provider = RawNindFoundationProvider::new_with_verifier(
        &installation,
        execution_plan,
        foundation_plan,
        fixture.input_raw.clone(),
        fixture.output_foundation.clone(),
        Arc::new(FixtureOutputVerifier {
            verification: fixture_verification(fixture.output_foundation.clone()),
        }),
    )
    .expect("configured provider");

    let receipt = ExecutionLease::issue("rawnind-lease-invalid".into(), execution)
        .expect("execution lease")
        .execute(&provider, &CancellationToken::default());

    assert!(matches!(
        receipt.outcome,
        RuntimeTerminalOutcome::Failed { .. }
    ));
    assert!(!fixture.output_foundation.exists());
}

#[cfg(unix)]
#[test]
fn real_public_materialization_publishes_then_reuses_the_verified_cache_when_supplied() {
    let variable_names = [
        "SHADOW_TEST_RAWNIND_PYTHON",
        "SHADOW_TEST_RAWNIND_SIDECAR",
        "SHADOW_TEST_RAWNIND_PACKAGE",
        "SHADOW_TEST_RAWNIND_GRAPH",
        "SHADOW_TEST_RAWNIND_RAW",
        "SHADOW_TEST_RAWNIND_RAW_SHA256",
    ];
    let Some(values) = variable_names
        .map(std::env::var_os)
        .into_iter()
        .collect::<Option<Vec<_>>>()
    else {
        eprintln!("skipping real RawNIND Rust acceptance; external paths were not supplied");
        return;
    };
    let python = PathBuf::from(&values[0]);
    let sidecar = PathBuf::from(&values[1]);
    let package = PathBuf::from(&values[2]);
    let graph = PathBuf::from(&values[3]);
    let raw = PathBuf::from(&values[4]);
    let raw_sha256 = values[5]
        .to_str()
        .expect("RAW SHA-256 must be UTF-8")
        .to_owned();
    validate_lower_sha256(&raw_sha256).expect("canonical RAW SHA-256");
    let fixture = RealFixture::new(&python, &sidecar);
    let manifest_path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../apps/desktop/providers/rawnind-foundation/model-manifest.json");
    let installation = verify_rawnind_foundation_installation(
        &fixture.launcher,
        &package,
        &graph,
        &manifest_path,
        &CancellationToken::default(),
    )
    .expect("real RawNIND installation");
    let raw_bytes = fs::metadata(&raw).expect("public RAW metadata").len();
    let store =
        FoundationArtifactStore::open(fixture.root.join("cache")).expect("foundation store");
    let first_execution = Fixture::execution(&installation, raw_sha256.clone(), raw_bytes);
    let first = materialize_rawnind_foundation(
        &store,
        &installation,
        "rawnind-real-public-first".into(),
        first_execution,
        &raw,
        &CancellationToken::default(),
    )
    .expect("first real materialization");
    let RawFoundationMaterializationOutcome::Ready(first) = first else {
        panic!("real RawNIND materialization did not become ready");
    };
    assert_eq!(
        first.disposition(),
        RawFoundationMaterializationDisposition::Published
    );
    assert!(first.runtime_receipt().is_some());
    assert_eq!(first.descriptor().raster_extent().width, 3908);
    assert_eq!(first.descriptor().raster_extent().height, 2600);
    assert_eq!(first.descriptor().source().source_file_sha256(), raw_sha256);
    let first_descriptor = first.descriptor().clone();
    let first_path = first.path().to_path_buf();
    let verification = verify_foundation_artifact(&first_path).expect("Rust artifact verification");
    assert_eq!(
        first.descriptor().artifact().content_hash(),
        verification.file_sha256
    );
    assert_eq!(
        first.descriptor().provenance().cache_key_sha256(),
        verification.cache_key_sha256
    );
    assert_eq!(
        first.plan().cache_key_sha256(),
        verification.cache_key_sha256
    );

    let second_execution = Fixture::execution(&installation, raw_sha256, raw_bytes);
    let second = materialize_rawnind_foundation(
        &store,
        &installation,
        "rawnind-real-public-cache-hit".into(),
        second_execution,
        &raw,
        &CancellationToken::default(),
    )
    .expect("cached real materialization");
    let RawFoundationMaterializationOutcome::Ready(second) = second else {
        panic!("verified cache hit did not become ready");
    };
    assert_eq!(
        second.disposition(),
        RawFoundationMaterializationDisposition::ReusedVerified
    );
    assert!(second.runtime_receipt().is_none());
    assert_eq!(second.path(), first_path);
    assert_eq!(second.descriptor(), &first_descriptor);
}

#[derive(Debug)]
struct FixtureOutputVerifier {
    verification: FoundationArtifactVerification,
}

impl FoundationOutputVerifier for FixtureOutputVerifier {
    fn verify(
        &self,
        _path: &Path,
    ) -> Result<FoundationArtifactVerification, RawNindFoundationOutputVerificationError> {
        Ok(self.verification.clone())
    }
}

fn fixture_verification(path: PathBuf) -> FoundationArtifactVerification {
    FoundationArtifactVerification {
        path,
        width: 6000,
        height: 4000,
        force_rggb_crop_sensor: [0, 0],
        source_sha256: "a".repeat(64),
        source_size_bytes: 17,
        source_pixel_contract_sha256: RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256.into(),
        model_package_sha256: RAWNIND_FOUNDATION_PACKAGE_SHA256.into(),
        model_graph_sha256: RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256.into(),
        implementation_revision: RAWNIND_FOUNDATION_IMPLEMENTATION_REVISION.into(),
        cache_key_sha256: "c".repeat(64),
        artifact_identity_sha256: "d".repeat(64),
        sequence_sha256: "e".repeat(64),
        payload_bytes: 288_000_000,
        file_bytes: 288_004_096,
        file_sha256: "f".repeat(64),
        stripes: vec![FoundationArtifactStripe {
            index: 0,
            y_start: 0,
            rows: 4000,
            offset: 4096,
            byte_length: 288_000_000,
            sha256: "1".repeat(64),
        }],
    }
}

fn fixture_receipt() -> FoundationReceipt {
    FoundationReceipt {
        cache_key_sha256: "c".repeat(64),
        artifact_identity_sha256: "d".repeat(64),
        file_sha256: "f".repeat(64),
        width: 6000,
        height: 4000,
        runtime_version: "1.24.4".into(),
    }
}

fn fixture_plan() -> RawNindFoundationPlan {
    RawNindFoundationPlan {
        cache_key_sha256: "c".repeat(64),
        source_sha256: "a".repeat(64),
        source_size_bytes: 17,
        source_pixel_contract_sha256: RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256.into(),
        raster_extent: RasterExtent::new(6000, 4000).unwrap(),
        runtime_version: "1.24.4".into(),
    }
}

#[cfg(unix)]
struct Fixture {
    root: PathBuf,
    executable: PathBuf,
    model_package: PathBuf,
    model_graph: PathBuf,
    manifest_path: PathBuf,
    input_raw: PathBuf,
    output_foundation: PathBuf,
}

#[cfg(unix)]
impl Fixture {
    fn new(label: &str) -> Self {
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "shadow-rawnind-foundation-provider-{label}-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).expect("fixture root");
        let executable = write_fake_provider(&root, label);

        let model_package = root.join("rawdenoise-nind.dtmodel");
        let model_graph = root.join("model_bayer.onnx");
        let manifest_path = root.join("model-manifest.json");
        let input_raw = root.join("input.raw");
        let output_foundation = root.join("output.shadowrawf");
        fs::write(&model_package, b"fixture package").expect("package");
        fs::write(&model_graph, b"fixture graph").expect("graph");
        fs::write(&manifest_path, CHECKED_IN_MANIFEST).expect("manifest");
        fs::write(&input_raw, b"seventeen bytes!!").expect("RAW input");
        assert_eq!(fs::metadata(&input_raw).unwrap().len(), 17);

        Self {
            root,
            executable,
            model_package,
            model_graph,
            manifest_path,
            input_raw,
            output_foundation,
        }
    }

    fn installation(&self) -> VerifiedRawNindFoundationInstallation {
        verify_rawnind_foundation_installation(
            &self.executable,
            &self.model_package,
            &self.model_graph,
            &self.manifest_path,
            &CancellationToken::default(),
        )
        .expect("verified fake installation")
    }

    fn foundation_plan(
        &self,
        installation: &VerifiedRawNindFoundationInstallation,
    ) -> RawNindFoundationPlan {
        plan_rawnind_foundation(
            installation,
            &self.input_raw,
            &"a".repeat(64),
            17,
            &CancellationToken::default(),
        )
        .expect("planned fake foundation")
    }

    fn execution(
        installation: &VerifiedRawNindFoundationInstallation,
        source_sha256: String,
        source_size_bytes: u64,
    ) -> AdmittedExecution {
        let request = crate::AiJobRequest {
            contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
            request_id: "rawnind-request-1".into(),
            generation: 9,
            task: AiTaskKind::MaterializeRawFoundation,
            target: ObservationTarget::Photo {
                photo_id: PhotoId::from_str("018f3ec1-6219-7df2-a52d-f744c4f88533")
                    .expect("photo id"),
            },
            priority: TaskPriority::CurrentInput,
            privacy: PrivacyClass::Personal,
            inputs: vec![ArtifactReference {
                role: InputRole::RawFile,
                content_hash: source_sha256,
                byte_len: source_size_bytes,
                media_type: "image/x-raw".into(),
                privacy: PrivacyClass::Personal,
            }],
            parameters: AiTaskParameters::RawFoundation,
            estimate: route_estimate(),
        };
        let binding = LocalExecutionBinding {
            execution_id: "rawnind-execution-1".into(),
            request,
            provider: installation.provider_identity().clone(),
            route_estimate: route_estimate(),
            fallback: FallbackDisclosure::Primary,
        };
        let hardware = HardwareProfile {
            platform: Platform::MacOs,
            total_system_ram_bytes: 16 * 1024 * 1024 * 1024,
            available_system_ram_bytes: 12 * 1024 * 1024 * 1024,
            logical_cpu_threads: 8,
            on_battery: false,
            backends: vec![ExecutionBackend {
                id: installation.backend_id().into(),
                kind: BackendKind::Cpu,
                available: true,
                supported_precisions: vec![NumericPrecision::Float32],
                total_device_memory_bytes: None,
                available_device_memory_bytes: None,
                maximum_concurrent_sessions: 1,
                active_sessions: 0,
            }],
        };
        let policy = ResourcePolicy {
            maximum_ai_system_ram_bytes: 8 * 1024 * 1024 * 1024,
            reserved_system_ram_bytes: 512 * 1024 * 1024,
            maximum_ai_cpu_threads: 8,
            maximum_device_memory_percent: 80,
            on_battery: OnBatteryPolicy::PauseBackground,
        };
        let availability = ModelAvailability {
            local: LocalModelAvailability::Installed {
                artifact_set_blake3: installation
                    .manifest()
                    .artifact_set
                    .inventory_blake3
                    .clone(),
                license_accepted: true,
            },
        };
        match admit_local_execution(
            binding,
            installation.manifest(),
            &hardware,
            policy,
            availability,
        )
        .expect("valid admission")
        {
            LocalExecutionAdmission::Admitted { execution } => *execution,
            LocalExecutionAdmission::Deferred { blockers } => {
                panic!("fixture admission deferred: {blockers:?}")
            }
        }
    }
}

#[cfg(unix)]
fn write_fake_provider(root: &Path, behavior: &str) -> PathBuf {
    use std::os::unix::fs::PermissionsExt;

    let executable = root.join("fake-provider.sh");
    fs::write(
        &executable,
        format!(
            r#"#!/bin/sh
behavior={behavior}
output=
verify=0
plan=0
run=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --verify-model)
            verify=1
            shift
            ;;
        --run)
            run=1
            shift
            ;;
        --plan)
            plan=1
            shift
            ;;
        --output-foundation)
            output="$2"
            shift 2
            ;;
        *)
            shift
            if [ "$#" -gt 0 ] && [ "${{1#--}}" = "$1" ]; then
                shift
            fi
            ;;
    esac
done
if [ "$verify" -eq 1 ]; then
    echo "{model_prefix} package_sha256={package} graph_sha256={graph} runtime_version=1.24.4"
    exit 0
fi
if [ "$plan" -eq 1 ]; then
    echo "{plan_prefix} cache_key_sha256={cache} source_sha256={source} source_size_bytes=17 source_pixel_contract_sha256={pixel_contract} width=6000 height=4000 runtime_version=1.24.4"
    exit 0
fi
if [ "$run" -eq 1 ] && [ -n "$output" ]; then
    : > "$output"
    if [ "$behavior" = "cancel" ]; then
        while :; do :; done
    fi
    if [ "$behavior" = "invalid" ]; then
        echo "invalid-success-receipt"
        exit 0
    fi
    echo "{run_prefix} cache_key_sha256={cache} artifact_identity_sha256={artifact} file_sha256={file} width=6000 height=4000 runtime_version=1.24.4"
    exit 0
fi
exit 2
"#,
            model_prefix = RAWNIND_FOUNDATION_MODEL_RECEIPT_PREFIX,
            package = RAWNIND_FOUNDATION_PACKAGE_SHA256,
            graph = RAWNIND_FOUNDATION_BAYER_GRAPH_SHA256,
            plan_prefix = RAWNIND_FOUNDATION_PLAN_RECEIPT_PREFIX,
            run_prefix = RAWNIND_FOUNDATION_RECEIPT_PREFIX,
            cache = "c".repeat(64),
            source = "a".repeat(64),
            pixel_contract = RAWNIND_FOUNDATION_SOURCE_PIXEL_CONTRACT_SHA256,
            artifact = "d".repeat(64),
            file = "f".repeat(64),
        ),
    )
    .expect("fake provider");
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o700);
    fs::set_permissions(&executable, permissions).expect("executable permission");
    executable
}

#[cfg(unix)]
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

#[cfg(unix)]
struct RealFixture {
    root: PathBuf,
    launcher: PathBuf,
}

#[cfg(unix)]
impl RealFixture {
    fn new(python: &Path, sidecar: &Path) -> Self {
        use std::os::unix::fs::PermissionsExt;

        assert!(python.is_file(), "pinned Python runtime");
        assert!(sidecar.is_file(), "RawNIND reference sidecar");
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "shadow-rawnind-real-provider-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).expect("real fixture root");
        let launcher = root.join("provider-launcher.sh");
        fs::write(
            &launcher,
            format!(
                "#!/bin/sh\nexec {} {} \"$@\"\n",
                shell_single_quote(python),
                shell_single_quote(sidecar),
            ),
        )
        .expect("real provider launcher");
        let mut permissions = fs::metadata(&launcher).unwrap().permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(&launcher, permissions).expect("launcher permission");
        Self { root, launcher }
    }
}

#[cfg(unix)]
impl Drop for RealFixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

#[cfg(unix)]
fn shell_single_quote(path: &Path) -> String {
    format!("'{}'", path.to_string_lossy().replace('\'', "'\"'\"'"))
}

fn route_estimate() -> ResourceEstimate {
    ResourceEstimate {
        peak_system_ram_bytes: 2 * 1024 * 1024 * 1024,
        peak_device_memory_bytes: 0,
        cpu_threads: 4,
        scratch_disk_bytes: 1024 * 1024 * 1024,
        upload_bytes: 0,
        estimated_duration_ms: Some(120_000),
    }
}
