#![cfg(unix)]

use std::{
    fs,
    os::unix::fs::PermissionsExt as _,
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};

use shadow_ai::{
    ExecutionBackend, HardwareProfile, MaskPointPolarity, NumericPrecision, ResourceEstimate,
    ResourcePolicy,
};
use shadow_core::DerivedRasterStageOutcome;

use super::*;
use crate::subject_mask_service::{SubjectMaskService, SubjectMaskServiceError};

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[test]
fn prepared_input_is_request_private_and_removed_on_drop() {
    let fixture = Fixture::new("prepared-input");
    let runtime = SubjectMaskRuntime::new(
        &fixture.executable,
        &fixture.model_directory,
        &fixture.manifest_path,
        &fixture.scratch_root,
    )
    .unwrap();
    let prepared = runtime
        .prepare_input_jpeg(b"bounded JPEG bytes")
        .expect("prepare input");
    let path = prepared.path().to_path_buf();

    assert_eq!(fs::read(&path).unwrap(), b"bounded JPEG bytes");
    drop(prepared);
    assert!(!path.exists());
}

#[test]
fn empty_prepared_input_is_rejected_without_creating_scratch() {
    let fixture = Fixture::new("empty-input");
    let runtime = SubjectMaskRuntime::new(
        &fixture.executable,
        &fixture.model_directory,
        &fixture.manifest_path,
        &fixture.scratch_root,
    )
    .unwrap();

    assert!(matches!(
        runtime.prepare_input_jpeg(&[]),
        Err(SubjectMaskRuntimeError::InputSize(0))
    ));
    assert_eq!(count_files(&fixture.scratch_root), 0);
}

#[cfg(target_os = "macos")]
#[test]
fn verified_local_route_stages_and_previews_one_point_mask_without_applying_it() {
    let fixture = Fixture::new("stage");
    let runtime = SubjectMaskRuntime::new(
        &fixture.executable,
        &fixture.model_directory,
        &fixture.manifest_path,
        &fixture.scratch_root,
    )
    .unwrap();
    let store = FilesystemDerivedRasterStore::open(&fixture.store_root).unwrap();
    let receipt = runtime
        .stage_with_resources(
            &store,
            SubjectMaskInvocation {
                request_id: "request-11".into(),
                promotion_id: "promotion-11".into(),
                generation: 11,
                photo_id: "018f3ec1-6219-7df2-a52d-f744c4f88533".into(),
                original_space_input_jpeg: fixture.input_jpeg.clone(),
                coordinate_extent: RasterExtent::new(1200, 800).unwrap(),
                points: vec![MaskPromptPoint {
                    x: UnitInterval::new(0.5).unwrap(),
                    y: UnitInterval::new(0.5).unwrap(),
                    polarity: MaskPointPolarity::Foreground,
                }],
            },
            &CancellationToken::default(),
            &resources(),
        )
        .expect("staged subject mask");

    assert_eq!(receipt.generation, 11);
    let DerivedRasterStageOutcome::Staged(staged) = receipt.outcome else {
        panic!("expected staged subject-mask proposal");
    };
    assert_eq!(count_files(&fixture.store_root.join("objects")), 0);
    assert_eq!(count_files(&fixture.store_root.join("proposals")), 1);

    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let proposal_token = service.register_proposal(staged).unwrap();
    let first = service.proposal_preview(proposal_token).unwrap();
    let second = service.proposal_preview(proposal_token).unwrap();
    assert_eq!(
        first, second,
        "preview reads do not consume apply authority"
    );
    assert_eq!(first.generation, 11);
    assert_eq!(first.raster_extent, RasterExtent::new(256, 256).unwrap());
    assert_eq!(first.samples.len(), 256 * 256);
    assert!(first.samples.iter().all(|sample| *sample == 0));
    assert_eq!(count_files(&fixture.store_root.join("objects")), 0);

    service.discard_proposal(proposal_token).unwrap();
    assert!(matches!(
        service.proposal_preview(proposal_token),
        Err(SubjectMaskServiceError::UnknownProposal(token)) if token == proposal_token
    ));
}

#[cfg(target_os = "macos")]
#[test]
fn configured_real_route_stages_distinct_point_refinements() {
    let Some(provider) = std::env::var_os("SHADOW_SAM2_COREML_PROVIDER_PATH") else {
        return;
    };
    let Some(model_directory) = std::env::var_os("SHADOW_SAM2_COREML_MODEL_DIR") else {
        return;
    };
    let Some(manifest) = std::env::var_os("SHADOW_SAM2_COREML_MANIFEST_PATH") else {
        return;
    };
    let Some(input_jpeg) = std::env::var_os("SHADOW_SAM2_COREML_ACCEPTANCE_INPUT_JPEG") else {
        return;
    };

    let fixture = Fixture::new("real-acceptance");
    let runtime = SubjectMaskRuntime::new(
        PathBuf::from(provider),
        PathBuf::from(model_directory),
        PathBuf::from(manifest),
        &fixture.scratch_root,
    )
    .unwrap();
    let store = FilesystemDerivedRasterStore::open(&fixture.store_root).unwrap();
    let input_jpeg = PathBuf::from(input_jpeg);
    let foreground = MaskPromptPoint {
        x: UnitInterval::new(0.49).unwrap(),
        y: UnitInterval::new(0.55).unwrap(),
        polarity: MaskPointPolarity::Foreground,
    };
    let background = MaskPromptPoint {
        x: UnitInterval::new(0.64).unwrap(),
        y: UnitInterval::new(0.55).unwrap(),
        polarity: MaskPointPolarity::Background,
    };

    let first = runtime
        .stage_with_resources(
            &store,
            real_invocation(
                "real-request-1",
                "real-promotion-1",
                1,
                &input_jpeg,
                vec![foreground],
            ),
            &CancellationToken::default(),
            &resources(),
        )
        .expect("stage real one-point mask");
    let second = runtime
        .stage_with_resources(
            &store,
            real_invocation(
                "real-request-2",
                "real-promotion-2",
                2,
                &input_jpeg,
                vec![foreground, background],
            ),
            &CancellationToken::default(),
            &resources(),
        )
        .expect("stage real refined mask");

    let first = match first.outcome {
        DerivedRasterStageOutcome::Staged(first) => first,
        other => panic!("expected first real proposal, got {other:?}"),
    };
    let second = match second.outcome {
        DerivedRasterStageOutcome::Staged(second) => second,
        other => panic!("expected refined real proposal, got {other:?}"),
    };
    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let first_token = service.register_proposal(first).unwrap();
    let second_token = service.register_proposal(second).unwrap();
    let first_preview = service.proposal_preview(first_token).unwrap();
    let second_preview = service.proposal_preview(second_token).unwrap();
    assert!(first_preview.samples.iter().any(|sample| *sample != 0));
    assert!(second_preview.samples.iter().any(|sample| *sample != 0));
    assert_ne!(first_preview.samples, second_preview.samples);
}

fn real_invocation(
    request_id: &str,
    promotion_id: &str,
    generation: u64,
    input_jpeg: &Path,
    points: Vec<MaskPromptPoint>,
) -> SubjectMaskInvocation {
    SubjectMaskInvocation {
        request_id: request_id.into(),
        promotion_id: promotion_id.into(),
        generation,
        photo_id: "018f3ec1-6219-7df2-a52d-f744c4f88533".into(),
        original_space_input_jpeg: input_jpeg.to_path_buf(),
        coordinate_extent: RasterExtent::new(4288, 2848).unwrap(),
        points,
    }
}

fn resources() -> SubjectMaskRuntimeResources {
    let estimate = ResourceEstimate {
        peak_system_ram_bytes: GIBIBYTE,
        peak_device_memory_bytes: 0,
        cpu_threads: 2,
        scratch_disk_bytes: 256 * MEBIBYTE,
        upload_bytes: 0,
        estimated_duration_ms: Some(1_500),
    };
    SubjectMaskRuntimeResources {
        hardware: HardwareProfile {
            platform: Platform::MacOs,
            total_system_ram_bytes: 16 * GIBIBYTE,
            available_system_ram_bytes: 8 * GIBIBYTE,
            logical_cpu_threads: 8,
            on_battery: false,
            backends: vec![ExecutionBackend {
                id: "test-coreml".into(),
                kind: BackendKind::CoreMl,
                available: true,
                supported_precisions: vec![NumericPrecision::Float16],
                total_device_memory_bytes: None,
                available_device_memory_bytes: None,
                maximum_concurrent_sessions: 1,
                active_sessions: 0,
            }],
        },
        policy: ResourcePolicy {
            maximum_ai_system_ram_bytes: 8 * GIBIBYTE,
            reserved_system_ram_bytes: GIBIBYTE,
            maximum_ai_cpu_threads: 4,
            maximum_device_memory_percent: 75,
            on_battery: OnBatteryPolicy::Continue,
        },
        route_estimate: estimate,
    }
}

fn count_files(root: &PathBuf) -> usize {
    if !root.exists() {
        return 0;
    }
    fs::read_dir(root)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .map(|path| if path.is_dir() { count_files(&path) } else { 1 })
        .sum()
}

struct Fixture {
    root: PathBuf,
    executable: PathBuf,
    model_directory: PathBuf,
    manifest_path: PathBuf,
    scratch_root: PathBuf,
    store_root: PathBuf,
    input_jpeg: PathBuf,
}

impl Fixture {
    fn new(label: &str) -> Self {
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "shadow-subject-mask-runtime-{label}-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        let executable = root.join("fake-provider.sh");
        fs::write(
            &executable,
            r#"#!/bin/sh
output=
points=0
verify=0
serve=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --verify-model)
            verify=1
            shift
            ;;
        --serve)
            serve=1
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
if [ "$serve" -eq 1 ]; then
    printf '{"protocol":1,"type":"ready","revision":"883f5787eb0be35ce6965907a8bc1f5320a5a02e"}\n'
    while IFS= read -r line; do
        id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
        case "$line" in
            *'"op":"load_image"'*)
                printf '{"protocol":1,"type":"response","id":%s,"op":"load_image","ok":true,"cache_hit":false}\n' "$id"
                ;;
            *'"op":"predict"'*)
                output=$(printf '%s' "$line" | sed -n 's/.*"output_mask":"\([^"]*\)".*/\1/p')
                dd if=/dev/zero of="$output" bs=65536 count=1 2>/dev/null
                printf '{"protocol":1,"type":"response","id":%s,"op":"predict","ok":true,"width":256,"height":256,"score":0.9,"points":1}\n' "$id"
                ;;
        esac
    done
    exit 0
fi
dd if=/dev/zero of="$output" bs=65536 count=1 2>/dev/null
printf 'shadow-sam2-coreml-mask-v1 width=256 height=256 score=0.9 points=%s\n' "$points"
"#,
        )
        .unwrap();
        let mut permissions = fs::metadata(&executable).unwrap().permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(&executable, permissions).unwrap();

        let model_directory = root.join("models");
        fs::create_dir(&model_directory).unwrap();
        for package in [
            "SAM2_1SmallImageEncoderFLOAT16.mlpackage",
            "SAM2_1SmallPromptEncoderFLOAT16.mlpackage",
            "SAM2_1SmallMaskDecoderFLOAT16.mlpackage",
        ] {
            fs::create_dir(model_directory.join(package)).unwrap();
        }
        let manifest_path = root.join("model-manifest.json");
        fs::write(
            &manifest_path,
            include_str!("../../../../apps/desktop/providers/sam2-coreml/model-manifest.json"),
        )
        .unwrap();
        let input_jpeg = root.join("input.jpg");
        fs::write(&input_jpeg, b"bounded fixture JPEG").unwrap();
        Self {
            scratch_root: root.join("scratch"),
            store_root: root.join("derived-rasters"),
            root,
            executable,
            model_directory,
            manifest_path,
            input_jpeg,
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}
