use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

use super::*;

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

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
