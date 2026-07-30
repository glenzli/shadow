use std::{
    fs,
    path::PathBuf,
    process::{Command, Stdio},
    sync::atomic::{AtomicU64, Ordering},
    thread,
    time::Duration,
};

use shadow_domain::MaskCoordinateSpace;

use super::*;
use crate::{NumericPrecision, RunPlan, UnitInterval};

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[test]
fn receipt_requires_the_exact_mask_and_prompt_contract() {
    assert!(
        parse_receipt(
            b"shadow-sam2-coreml-mask-v1 width=256 height=256 score=0.900879 points=2\n",
            2
        )
        .is_ok()
    );
    assert!(
        parse_receipt(
            b"shadow-sam2-coreml-mask-v1 width=256 height=256 score=0.900879 points=1\n",
            2
        )
        .is_err()
    );
    assert!(
        parse_receipt(
            b"shadow-sam2-coreml-mask-v1 width=512 height=256 score=0.900879 points=2\n",
            2
        )
        .is_err()
    );
    assert!(parse_model_receipt(
        b"shadow-sam2-coreml-model-v1 revision=883f5787eb0be35ce6965907a8bc1f5320a5a02e files=9\n"
    )
    .is_ok());
}

#[test]
fn checked_in_manifest_is_the_exact_admitted_artifact_set() {
    let fixture = Fixture::new("manifest");
    let manifest = load_exact_manifest(&fixture.manifest_path).expect("exact SAM manifest");
    assert_eq!(manifest.model_id, SAM2_COREML_MODEL_ID);
    assert_eq!(
        manifest.artifact_set.inventory_blake3,
        SAM2_COREML_ARTIFACT_SET_BLAKE3
    );
}

#[cfg(target_os = "macos")]
#[test]
fn native_preflight_issues_installation_authority() {
    let fixture = Fixture::new("preflight");
    let installation = verify_sam2_coreml_installation(
        &fixture.executable,
        &fixture.model_directory,
        &fixture.manifest_path,
        &CancellationToken::default(),
    )
    .expect("verified installation");
    assert_eq!(
        installation.manifest().artifact_set.inventory_blake3,
        SAM2_COREML_ARTIFACT_SET_BLAKE3
    );
}

#[cfg(target_os = "macos")]
#[test]
fn fake_sidecar_produces_a_verified_multi_point_soft_mask() {
    let fixture = Fixture::new("success");
    let provider = fixture.provider();
    let parameters = point_parameters();

    let outcome = provider.execute_subject_mask(&parameters, &CancellationToken::default());
    let SidecarOutcome::Succeeded(AiGeneratedPayload::SoftMask(mask)) = outcome else {
        panic!("fake provider must produce a soft mask");
    };
    assert_eq!(mask.raster_extent, RasterExtent::new(256, 256).unwrap());
    assert_eq!(mask.coordinate_extent, parameters.coordinate_extent);
    assert_eq!(mask.coordinate_space, MaskCoordinateSpace::Original);
    assert_eq!(mask.artifact.byte_len(), 65_536);
    assert_eq!(
        fs::metadata(provider.proposal_path()).unwrap().len(),
        65_536
    );
}

#[cfg(unix)]
#[test]
fn in_flight_cancellation_terminates_the_child() {
    let cancellation = CancellationToken::default();
    let cancellation_for_thread = cancellation.clone();
    let mut command = Command::new("/bin/sh");
    command
        .arg("-c")
        .arg("sleep 5")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let child = command.spawn().expect("spawn cancellable child");
    let canceller = thread::spawn(move || {
        thread::sleep(Duration::from_millis(40));
        cancellation_for_thread.cancel();
    });

    assert!(matches!(
        wait_with_bounded_output(child, &cancellation, Duration::from_millis(5)),
        Err(ProcessFailure::Cancelled)
    ));
    canceller.join().expect("canceller");
}

fn point_parameters() -> SubjectMaskParameters {
    SubjectMaskParameters {
        prompt: MaskPrompt::Points {
            points: vec![
                crate::MaskPromptPoint {
                    x: UnitInterval::new(0.5).unwrap(),
                    y: UnitInterval::new(0.5).unwrap(),
                    polarity: MaskPointPolarity::Foreground,
                },
                crate::MaskPromptPoint {
                    x: UnitInterval::new(0.75).unwrap(),
                    y: UnitInterval::new(0.5).unwrap(),
                    polarity: MaskPointPolarity::Background,
                },
            ],
        },
        coordinate_space: MaskCoordinateSpace::Original,
        coordinate_extent: RasterExtent::new(1200, 800).unwrap(),
        edge_refinement: UnitInterval::new(0.5).unwrap(),
        maximum_candidates: 3,
    }
}

struct Fixture {
    root: PathBuf,
    executable: PathBuf,
    model_directory: PathBuf,
    manifest_path: PathBuf,
    input_jpeg: PathBuf,
    output_mask: PathBuf,
}

impl Fixture {
    fn new(label: &str) -> Self {
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "shadow-sam2-coreml-provider-{label}-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).expect("fixture root");
        let executable = root.join("fake-provider.sh");
        fs::write(
            &executable,
            r#"#!/bin/sh
output=
points=0
verify=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --verify-model)
            verify=1
            shift
            ;;
        --output-mask)
            output="$2"
            shift 2
            ;;
        --point)
            points=$((points + 1))
            shift 4
            ;;
        *)
            shift 2
            ;;
    esac
done
if [ "$verify" -eq 1 ]; then
    printf 'shadow-sam2-coreml-model-v1 revision=883f5787eb0be35ce6965907a8bc1f5320a5a02e files=9\n'
    exit 0
fi
dd if=/dev/zero of="$output" bs=65536 count=1 2>/dev/null
printf 'shadow-sam2-coreml-mask-v1 width=256 height=256 score=0.9 points=%s\n' "$points"
"#,
        )
        .expect("fake provider");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt as _;
            let mut permissions = fs::metadata(&executable).unwrap().permissions();
            permissions.set_mode(0o700);
            fs::set_permissions(&executable, permissions).unwrap();
        }

        let model_directory = root.join("models");
        fs::create_dir(&model_directory).unwrap();
        for package in MODEL_PACKAGES {
            fs::create_dir(model_directory.join(package)).unwrap();
        }
        let manifest_path = root.join("model-manifest.json");
        fs::write(
            &manifest_path,
            include_str!("../../../../../apps/desktop/providers/sam2-coreml/model-manifest.json"),
        )
        .unwrap();
        let input_jpeg = root.join("input.jpg");
        fs::write(&input_jpeg, b"fixture").unwrap();
        Self {
            output_mask: root.join("output.gray8"),
            root,
            executable,
            model_directory,
            manifest_path,
            input_jpeg,
        }
    }

    fn provider(&self) -> Sam2CoreMlSidecarProvider {
        let plan = RunPlan {
            backend_id: "coreml-test".into(),
            backend_kind: BackendKind::CoreMl,
            precision: NumericPrecision::Float16,
            cpu_threads: 1,
            reserved_system_ram_bytes: 512 * 1024 * 1024,
            reserved_device_memory_bytes: 0,
        };
        let manifest = load_exact_manifest(&self.manifest_path).unwrap();
        let installation = VerifiedSam2CoreMlInstallation {
            model_identity: model_identity(&manifest),
            manifest,
            executable: self.executable.clone(),
            model_directory: self.model_directory.clone(),
            manifest_path: self.manifest_path.clone(),
        };
        Sam2CoreMlSidecarProvider::new(
            &installation,
            ExecutionPlanIdentity::from_plan(plan).unwrap(),
            &self.input_jpeg,
            &self.output_mask,
        )
        .expect("provider")
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}
