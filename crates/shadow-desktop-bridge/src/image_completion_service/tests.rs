use super::*;

#[test]
fn placement_is_original_space_and_non_degenerate() {
    let placement = ImageCompletionPlacement {
        bounds_left: UnitInterval::new(0.1).unwrap(),
        bounds_top: UnitInterval::new(0.2).unwrap(),
        bounds_right: UnitInterval::new(0.7).unwrap(),
        bounds_bottom: UnitInterval::new(0.8).unwrap(),
    };
    assert!(placement.bounds_left < placement.bounds_right);
    assert!(placement.bounds_top < placement.bounds_bottom);
}

#[test]
fn cancellation_follows_candidate_preview_after_generation() {
    let directory = tempfile::tempdir().unwrap();
    let service = ImageCompletionService::open(directory.path()).unwrap();
    let job = service.begin_job().unwrap();
    service.attach_preview_render(job, 41).unwrap();
    service.replace_preview_render(job, 42).unwrap();
    assert_eq!(service.cancel_job(job).unwrap(), Some(42));
    assert!(service.cancellation(job).unwrap().is_cancelled());
    assert!(matches!(
        service.replace_preview_render(job, 43),
        Err(ImageCompletionServiceError::JobCancelled(_))
    ));
    service.finish_job(job).unwrap();
}
