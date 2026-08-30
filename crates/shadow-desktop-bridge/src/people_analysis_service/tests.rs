use super::*;

fn report() -> PeopleAnalysisReport {
    PeopleAnalysisReport {
        analyzed_photos: 0,
        detected_faces: 0,
        embedded_faces: 0,
        truncated: false,
        skipped: Default::default(),
        grouping: shadow_ai::AnonymousPeopleGroupingPlan {
            grouping_revision: "test".into(),
            policy: shadow_ai::AnonymousPersonGroupingPolicy::new(0.35, 2).expect("valid policy"),
            groups: Vec::new(),
            ungrouped: Vec::new(),
        },
        group_previews: Vec::new(),
    }
}

#[test]
fn progress_is_monotonic_and_ready_jobs_retire() {
    let service = PeopleAnalysisService::new();
    let token = service.begin_job(12).expect("begin job");
    let outcome = service
        .execute_job(token, |control| -> Result<_, &'static str> {
            control.publish(PeopleAnalysisProgress {
                phase: PeopleAnalysisPhase::Reviewing,
                analyzed_photos: 3,
                maximum_photos: 12,
                detected_faces: 2,
                compared_faces: 1,
            });
            control.publish(PeopleAnalysisProgress {
                phase: PeopleAnalysisPhase::Grouping,
                analyzed_photos: 3,
                maximum_photos: 12,
                detected_faces: 2,
                compared_faces: 1,
            });
            Ok(report())
        })
        .expect("execute job");
    assert!(matches!(outcome, PeopleAnalysisJobOutcome::Ready(_)));
    let snapshot = service.snapshot(token).expect("ready snapshot");
    assert_eq!(snapshot.phase, PeopleAnalysisJobPhase::Ready);
    assert_eq!(snapshot.analyzed_photos, 3);
    assert!(!service.cancel_job(token).expect("late cancellation"));
    service.retire_job(token).expect("retire job");
    assert!(matches!(
        service.snapshot(token),
        Err(PeopleAnalysisServiceError::UnknownJob(_))
    ));
}

#[test]
fn cancellation_wins_before_provider_work_starts() {
    let service = PeopleAnalysisService::new();
    let token = service.begin_job(8).expect("begin job");
    assert!(service.cancel_job(token).expect("cancel job"));
    let outcome = service
        .execute_job(token, |_control| -> Result<_, &'static str> {
            panic!("cancelled job must not enter provider work")
        })
        .expect("execute cancelled job");
    assert!(matches!(outcome, PeopleAnalysisJobOutcome::Cancelled));
    assert_eq!(
        service.snapshot(token).expect("cancelled snapshot").phase,
        PeopleAnalysisJobPhase::Cancelled
    );
}
