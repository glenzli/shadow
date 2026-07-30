#![cfg(unix)]

use std::{
    fs,
    os::unix::fs::PermissionsExt as _,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
    thread,
    time::{Duration, Instant},
};

use crate::{MaskPointPolarity, MaskPromptPoint, UnitInterval};

use super::*;
use crate::providers::sam2_coreml_sidecar::{
    MODEL_PACKAGES, VerifiedSam2CoreMlInstallation, load_exact_manifest, model_identity,
};

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[cfg(unix)]
#[test]
fn one_resident_process_reuses_the_loaded_image_for_point_refinement() {
    let fixture = Fixture::new("reuse", PredictionBehavior::Complete);
    let session = Sam2CoreMlResidentSession::new(&fixture.installation);
    let cancellation = CancellationToken::default();

    let first = session
        .predict(
            &fixture.input_jpeg,
            "same-rendered-input",
            &fixture.first_output,
            &points(),
            &cancellation,
        )
        .expect("first resident prediction");
    let second = session
        .predict(
            &fixture.input_jpeg,
            "same-rendered-input",
            &fixture.second_output,
            &points(),
            &cancellation,
        )
        .expect("refined resident prediction");

    assert_eq!(first.width, 256);
    assert_eq!(first.height, 256);
    assert_eq!(first.points, 2);
    assert_eq!(second.points, 2);
    assert_eq!(fs::metadata(&fixture.first_output).unwrap().len(), 65_536);
    assert_eq!(fs::metadata(&fixture.second_output).unwrap().len(), 65_536);
    assert_eq!(
        fs::read_to_string(&fixture.log_path).unwrap(),
        "load_image\npredict\npredict\n"
    );
}

#[cfg(unix)]
#[test]
fn changed_rendered_input_identity_replaces_the_resident_embedding() {
    let fixture = Fixture::new("replace-image", PredictionBehavior::Complete);
    let session = Sam2CoreMlResidentSession::new(&fixture.installation);
    let cancellation = CancellationToken::default();

    session
        .predict(
            &fixture.input_jpeg,
            "rendered-input-a",
            &fixture.first_output,
            &points(),
            &cancellation,
        )
        .expect("first resident prediction");
    session
        .predict(
            &fixture.input_jpeg,
            "rendered-input-b",
            &fixture.second_output,
            &points(),
            &cancellation,
        )
        .expect("replacement resident prediction");

    assert_eq!(
        fs::read_to_string(&fixture.log_path).unwrap(),
        "load_image\npredict\nload_image\npredict\n"
    );
}

#[cfg(unix)]
#[test]
fn cancellation_terminates_the_resident_process_and_returns_promptly() {
    let fixture = Fixture::new("cancel", PredictionBehavior::Hang);
    let session = Sam2CoreMlResidentSession::new(&fixture.installation);
    let cancellation = CancellationToken::default();
    let cancellation_for_thread = cancellation.clone();
    let canceller = thread::spawn(move || {
        thread::sleep(Duration::from_millis(50));
        cancellation_for_thread.cancel();
    });

    let started = Instant::now();
    assert!(matches!(
        session.predict(
            &fixture.input_jpeg,
            "cancelled-rendered-input",
            &fixture.first_output,
            &points(),
            &cancellation,
        ),
        Err(ResidentSessionFailure::Cancelled)
    ));
    assert!(started.elapsed() < Duration::from_secs(2));
    assert!(!fixture.first_output.exists());
    canceller.join().unwrap();
}

#[cfg(unix)]
#[test]
fn one_transport_failure_restarts_the_process_and_replays_the_complete_request() {
    let fixture = Fixture::new("recover", PredictionBehavior::CrashOnce);
    let session = Sam2CoreMlResidentSession::new(&fixture.installation);

    let receipt = session
        .predict(
            &fixture.input_jpeg,
            "recoverable-rendered-input",
            &fixture.first_output,
            &points(),
            &CancellationToken::default(),
        )
        .expect("resident request recovers once");

    assert_eq!(receipt.points, 2);
    assert_eq!(fs::metadata(&fixture.first_output).unwrap().len(), 65_536);
    assert_eq!(
        fs::read_to_string(&fixture.log_path).unwrap(),
        "load_image\npredict_crash\nload_image\npredict\n"
    );
}

fn points() -> Vec<MaskPromptPoint> {
    vec![
        MaskPromptPoint {
            x: UnitInterval::new(0.5).unwrap(),
            y: UnitInterval::new(0.5).unwrap(),
            polarity: MaskPointPolarity::Foreground,
        },
        MaskPromptPoint {
            x: UnitInterval::new(0.75).unwrap(),
            y: UnitInterval::new(0.5).unwrap(),
            polarity: MaskPointPolarity::Background,
        },
    ]
}

struct Fixture {
    root: PathBuf,
    installation: VerifiedSam2CoreMlInstallation,
    input_jpeg: PathBuf,
    first_output: PathBuf,
    second_output: PathBuf,
    log_path: PathBuf,
}

#[derive(Copy, Clone)]
enum PredictionBehavior {
    Complete,
    Hang,
    CrashOnce,
}

impl Fixture {
    fn new(label: &str, prediction_behavior: PredictionBehavior) -> Self {
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "shadow-sam2-coreml-resident-{label}-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        let log_path = root.join("protocol.log");
        let crash_marker = root.join("crashed-once");
        let executable = root.join("fake-resident-provider.sh");
        let completed_prediction = r#"output=$(printf '%s' "$line" | sed -n 's/.*"output_mask":"\([^"]*\)".*/\1/p')
            dd if=/dev/zero of="$output" bs=65536 count=1 2>/dev/null
            printf 'predict\n' >> "$log"
            printf '{"protocol":1,"type":"response","id":%s,"op":"predict","ok":true,"width":256,"height":256,"score":0.9,"points":2}\n' "$id""#;
        let prediction_body = match prediction_behavior {
            PredictionBehavior::Complete => completed_prediction.to_owned(),
            PredictionBehavior::Hang => "while :; do :; done".to_owned(),
            PredictionBehavior::CrashOnce => format!(
                r#"if [ ! -f '{marker}' ]; then
                touch '{marker}'
                printf 'predict_crash\n' >> "$log"
                exit 9
            fi
            {completed_prediction}"#,
                marker = crash_marker.display(),
            ),
        };
        fs::write(
            &executable,
            format!(
                r#"#!/bin/sh
log='{log}'
printf '{{"protocol":1,"type":"ready","revision":"{revision}"}}\n'
while IFS= read -r line; do
    id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
    case "$line" in
        *'"op":"load_image"'*)
            printf 'load_image\n' >> "$log"
            printf '{{"protocol":1,"type":"response","id":%s,"op":"load_image","ok":true,"cache_hit":false}}\n' "$id"
            ;;
        *'"op":"predict"'*)
            {prediction_body}
            ;;
        *'"op":"shutdown"'*)
            printf '{{"protocol":1,"type":"response","id":%s,"op":"shutdown","ok":true}}\n' "$id"
            exit 0
            ;;
    esac
done
"#,
                log = log_path.display(),
                revision = SAM2_COREML_EXACT_REVISION,
            ),
        )
        .unwrap();
        let mut permissions = fs::metadata(&executable).unwrap().permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(&executable, permissions).unwrap();

        let model_directory = root.join("models");
        fs::create_dir(&model_directory).unwrap();
        for package in MODEL_PACKAGES {
            fs::create_dir(model_directory.join(package)).unwrap();
        }
        let manifest_path = root.join("model-manifest.json");
        fs::write(
            &manifest_path,
            include_str!(
                "../../../../../../apps/desktop/providers/sam2-coreml/model-manifest.json"
            ),
        )
        .unwrap();
        let manifest = load_exact_manifest(&manifest_path).unwrap();
        let installation = VerifiedSam2CoreMlInstallation {
            model_identity: model_identity(&manifest),
            manifest,
            executable,
            model_directory,
            manifest_path,
        };
        let input_jpeg = root.join("input.jpg");
        fs::write(&input_jpeg, b"bounded JPEG fixture").unwrap();
        Self {
            first_output: root.join("first.gray8"),
            second_output: root.join("second.gray8"),
            root,
            installation,
            input_jpeg,
            log_path,
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}
