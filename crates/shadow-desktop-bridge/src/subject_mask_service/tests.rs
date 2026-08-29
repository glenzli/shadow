use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

use super::*;

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[test]
fn one_input_is_prepared_once_and_reused_by_later_prompt_jobs() {
    let fixture = Fixture::new("reuse-input");
    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let session = service.begin_input_session().unwrap();
    let identity = input_identity("photo-a");

    assert!(matches!(
        service.admit_input(session, &identity).unwrap(),
        SubjectMaskInputAdmission::Prepare
    ));
    assert!(matches!(
        service.admit_input(session, &identity),
        Err(SubjectMaskServiceError::InputPreparationInFlight(token)) if token == session
    ));

    let extent = RasterExtent::new(1_024, 683).unwrap();
    let prepared = service
        .complete_input_preparation(session, &identity, b"stable JPEG bytes".to_vec(), extent)
        .unwrap();
    let reused = match service.admit_input(session, &identity).unwrap() {
        SubjectMaskInputAdmission::Reuse(input) => input,
        SubjectMaskInputAdmission::Prepare => panic!("prepared input must be reused"),
    };

    assert!(Arc::ptr_eq(&prepared.bytes, &reused.bytes));
    assert_eq!(prepared.content_hash, reused.content_hash);
    assert_eq!(reused.coordinate_extent, extent);
    service.finish_input_session(session).unwrap();
    assert!(matches!(
        service.admit_input(session, &identity),
        Err(SubjectMaskServiceError::UnknownInputSession(token)) if token == session
    ));
}

#[test]
fn input_session_rejects_recipe_identity_changes() {
    let fixture = Fixture::new("identity-change");
    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let session = service.begin_input_session().unwrap();
    let first = input_identity("photo-a");
    let changed = input_identity("photo-b");

    assert!(matches!(
        service.admit_input(session, &first).unwrap(),
        SubjectMaskInputAdmission::Prepare
    ));
    assert!(matches!(
        service.admit_input(session, &changed),
        Err(SubjectMaskServiceError::InputIdentityChanged(token)) if token == session
    ));
}

#[test]
fn cancelled_preparation_can_be_retried_in_the_same_prompt_session() {
    let fixture = Fixture::new("retry-input");
    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let session = service.begin_input_session().unwrap();
    let identity = input_identity("photo-a");

    assert!(matches!(
        service.admit_input(session, &identity).unwrap(),
        SubjectMaskInputAdmission::Prepare
    ));
    service.abort_input_preparation(session, &identity).unwrap();
    assert!(matches!(
        service.admit_input(session, &identity).unwrap(),
        SubjectMaskInputAdmission::Prepare
    ));
}

#[test]
fn people_and_parsed_labels_are_cached_for_one_input_session() {
    let fixture = Fixture::new("people-cache");
    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let session = service.begin_input_session().unwrap();
    let identity = input_identity("photo-a");
    assert!(matches!(
        service.admit_input(session, &identity).unwrap(),
        SubjectMaskInputAdmission::Prepare
    ));
    service
        .complete_input_preparation(
            session,
            &identity,
            b"stable JPEG bytes".to_vec(),
            RasterExtent::new(2, 2).unwrap(),
        )
        .unwrap();

    let candidate = SubjectMaskPersonCandidate {
        bounding_box: shadow_ai::FaceBoundingBox {
            x: 0.0,
            y: 0.0,
            width: 2.0,
            height: 2.0,
        },
        confidence: 0.9,
        thumbnail_jpeg: Arc::from(&b"thumbnail"[..]),
    };
    let first = service
        .cache_people(session, &identity, vec![candidate.clone()])
        .unwrap();
    let reused = service
        .cache_people(session, &identity, Vec::new())
        .unwrap();
    assert_eq!(first.len(), 1);
    assert_eq!(reused.len(), 1);

    let parsed = ParsedSubjectMaskPerson::new(
        vec![1, 4, 5, 17],
        RasterExtent::new(2, 2).unwrap(),
        vision_provenance(),
    )
    .unwrap();
    let first_parsed = service
        .cache_parsed_person(session, &identity, 0, parsed)
        .unwrap();
    let reused_parsed = service
        .parsed_person(session, &identity, 0)
        .unwrap()
        .unwrap();
    assert!(Arc::ptr_eq(&first_parsed, &reused_parsed));
    assert_eq!(
        service
            .people_snapshot(session, &identity)
            .unwrap()
            .unwrap()[0]
            .available_regions,
        Some(first_parsed.available_regions)
    );
}

#[test]
fn cancellation_reaches_provider_and_returns_the_attached_preview_token() {
    let fixture = Fixture::new("cancel");
    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let job = service.begin_job().unwrap();
    let cancellation = service.cancellation(job).unwrap();
    service.attach_preview_render(job, 41).unwrap();

    assert_eq!(service.cancel_job(job).unwrap(), Some(41));
    assert!(cancellation.is_cancelled());
    service.finish_job(job).unwrap();
    assert!(matches!(
        service.cancellation(job),
        Err(SubjectMaskServiceError::UnknownJob(token)) if token == job
    ));
}

#[test]
fn cancelled_job_cannot_attach_a_late_preview() {
    let fixture = Fixture::new("late-preview");
    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let job = service.begin_job().unwrap();
    assert_eq!(service.cancel_job(job).unwrap(), None);
    assert!(matches!(
        service.attach_preview_render(job, 52),
        Err(SubjectMaskServiceError::JobCancelled(token)) if token == job
    ));
}

#[test]
fn one_preview_render_is_attached_to_each_job() {
    let fixture = Fixture::new("one-preview");
    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let job = service.begin_job().unwrap();
    service.attach_preview_render(job, 7).unwrap();
    assert!(matches!(
        service.attach_preview_render(job, 8),
        Err(SubjectMaskServiceError::PreviewAlreadyAttached(token)) if token == job
    ));
}

#[test]
fn terminal_receipt_retires_the_job_exactly_once() {
    let fixture = Fixture::new("complete");
    let service = SubjectMaskService::open(&fixture.store_root).unwrap();
    let job = service.begin_job().unwrap();
    let completion = service
        .complete_job(
            job,
            DerivedRasterStageReceipt {
                request_id: "request-complete".into(),
                generation: 9,
                usage: shadow_ai::RuntimeUsage::default(),
                outcome: DerivedRasterStageOutcome::Cancelled,
            },
        )
        .unwrap();

    assert!(matches!(completion, SubjectMaskCompletion::Cancelled));
    assert!(matches!(
        service.complete_job(
            job,
            DerivedRasterStageReceipt {
                request_id: "request-duplicate".into(),
                generation: 9,
                usage: shadow_ai::RuntimeUsage::default(),
                outcome: DerivedRasterStageOutcome::Cancelled,
            },
        ),
        Err(SubjectMaskServiceError::UnknownJob(token)) if token == job
    ));
}

fn input_identity(photo_id: &str) -> SubjectMaskInputIdentity {
    SubjectMaskInputIdentity {
        photo_id: photo_id.into(),
        source_path: format!("/{photo_id}.jpg"),
        base_commit_id: "base-commit".into(),
        grade_stack: GradeStackDraft::default(),
        target_grade_node_index: 0,
        target_grade_node_id: "target-node".into(),
    }
}

fn vision_provenance() -> shadow_ai::VisionProvenance {
    shadow_ai::VisionProvenance {
        job_id: "job".into(),
        provider: "provider".into(),
        deployment: "deployment".into(),
        model_build: "build".into(),
        artifact_sha256: "artifact".into(),
        preprocessing_identity: "pre".into(),
        postprocessing_identity: "post".into(),
        tokenizer: None,
        runtime: "runtime".into(),
        requested_execution_provider: "cpu".into(),
        actual_execution_provider: "cpu".into(),
        execution_provider_fallback_reason: None,
        precision: "fp32".into(),
    }
}

struct Fixture {
    root: PathBuf,
    store_root: PathBuf,
}

impl Fixture {
    fn new(label: &str) -> Self {
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "shadow-subject-mask-service-{label}-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        Self {
            store_root: root.join("derived-rasters"),
            root,
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}
