use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

use super::*;

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[test]
fn prepared_input_is_request_private_and_removed_on_drop() {
    let fixture = Fixture::new("prepared-input");
    let runtime =
        SubjectMaskRuntime::new(&fixture.scratch_root, None, &fixture.credential).unwrap();
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
    let runtime =
        SubjectMaskRuntime::new(&fixture.scratch_root, None, &fixture.credential).unwrap();

    assert!(matches!(
        runtime.prepare_input_jpeg(&[]),
        Err(SubjectMaskRuntimeError::InputSize(0))
    ));
    assert_eq!(count_files(&fixture.scratch_root), 0);
}

#[test]
fn face_region_composition_maps_only_the_selected_ontology_classes() {
    let extent = RasterExtent::new(4, 2).unwrap();
    let labels = [1, 4, 5, 10, 11, 12, 13, 17];

    assert_eq!(
        compose_face_region_mask(&labels, extent, FaceRegion::Eyes).unwrap(),
        vec![0, 255, 255, 0, 0, 0, 0, 0]
    );
    assert_eq!(
        compose_face_region_mask(&labels, extent, FaceRegion::LipsAndMouth).unwrap(),
        vec![0, 0, 0, 0, 255, 255, 255, 0]
    );
    assert_eq!(
        compose_face_region_mask(&labels, extent, FaceRegion::Hair).unwrap(),
        vec![0, 0, 0, 0, 0, 0, 0, 255]
    );
}

#[test]
fn absent_face_region_fails_instead_of_staging_an_empty_mask() {
    let extent = RasterExtent::new(2, 2).unwrap();
    assert!(matches!(
        compose_face_region_mask(&[1, 1, 1, 1], extent, FaceRegion::Accessories),
        Err(SubjectMaskRuntimeError::FaceRegionUnavailable)
    ));
}

struct Fixture {
    root: PathBuf,
    scratch_root: PathBuf,
    credential: PathBuf,
}

impl Fixture {
    fn new(label: &str) -> Self {
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "shadow-subject-mask-runtime-{label}-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        let credential = root.join("infer-runtime-shadow.token");
        fs::write(&credential, "test-token\n").unwrap();
        Self {
            scratch_root: root.join("scratch"),
            credential,
            root,
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
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
