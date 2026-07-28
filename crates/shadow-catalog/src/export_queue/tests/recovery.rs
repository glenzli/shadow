use crate::{
    AdvanceExportItem, Catalog, ExportFailure, ExportItemState, ExportJobState,
    MAX_EXPORT_JOB_PAGE_SIZE, NewExportItem,
};

use super::queue_fixtures::{enqueue, enqueue_items, output, seeded_item};

#[test]
fn startup_recovery_requeues_legacy_interrupted_rows_without_rewriting_snapshots() {
    let mut catalog = Catalog::open_in_memory().expect("catalog");
    let job = enqueue(&mut catalog);
    let claimed = catalog
        .claim_next_export_item(4)
        .expect("claim")
        .expect("one item");
    let interrupted = catalog
        .advance_export_item(&AdvanceExportItem {
            item_id: claimed.id,
            expected_state: ExportItemState::Preparing,
            next_state: ExportItemState::Interrupted,
            failure: Some(ExportFailure {
                code: "worker_interrupted".into(),
                message: "previous process stopped before atomic output commit".into(),
                retryable: true,
            }),
            receipt: None,
            now_ms: 5,
        })
        .expect("seed legacy interrupted row");
    assert_eq!(interrupted.state, ExportItemState::Interrupted);

    let recovery = catalog
        .recover_and_requeue_interrupted_export_items(6)
        .expect("recover interrupted row");
    assert_eq!(recovery.interrupted_item_count, 0);
    assert_eq!(recovery.requeued_item_count, 1);
    assert_eq!(recovery.queued_item_count, 1);
    let queued = catalog
        .export_item(interrupted.id)
        .expect("indexed item")
        .expect("queued item");
    assert_eq!(queued.state, ExportItemState::Queued);
    assert_eq!(queued.recipe_commit_id, interrupted.recipe_commit_id);
    assert_eq!(
        queued.recipe_snapshot_digest,
        interrupted.recipe_snapshot_digest
    );
    assert_eq!(
        queued.source_identity_digest,
        interrupted.source_identity_digest
    );
    assert_eq!(queued.render_plan_digest, interrupted.render_plan_digest);
    assert_eq!(queued.output, interrupted.output);
    assert_eq!(queued.attempt_count, interrupted.attempt_count);
    assert!(queued.started_at_ms.is_none());
    assert!(queued.finished_at_ms.is_none());
    assert!(queued.error.is_none());
    assert_eq!(
        catalog.export_job(job.id).expect("job").expect("job").state,
        ExportJobState::Queued
    );
}

#[test]
fn startup_recovery_is_not_limited_by_task_center_page_size() {
    let mut catalog = Catalog::open_in_memory().expect("catalog");
    let item = seeded_item(&mut catalog);
    let mut job_ids = Vec::with_capacity(MAX_EXPORT_JOB_PAGE_SIZE + 1);
    for index in 0..=MAX_EXPORT_JOB_PAGE_SIZE {
        let item = NewExportItem {
            output: output(&format!("/exports/recovery-{index}.jpg")),
            ..item.clone()
        };
        let now_ms = i64::try_from(index).expect("test timestamp") + 10;
        let job = enqueue_items(&mut catalog, vec![item], now_ms);
        let claimed = catalog
            .claim_next_export_item(now_ms + 1)
            .expect("claim queued item")
            .expect("one queued item");
        assert_eq!(claimed.state, ExportItemState::Preparing);
        job_ids.push(job.id);
    }

    let recovery = catalog
        .recover_and_requeue_interrupted_export_items(1_000)
        .expect("recover every active job");
    let expected = u64::try_from(MAX_EXPORT_JOB_PAGE_SIZE + 1).expect("test count");
    assert_eq!(recovery.interrupted_item_count, expected);
    assert_eq!(recovery.requeued_item_count, expected);
    assert_eq!(recovery.queued_item_count, expected);
    for index in [0, MAX_EXPORT_JOB_PAGE_SIZE / 2, MAX_EXPORT_JOB_PAGE_SIZE] {
        let job_id = job_ids[index];
        assert_eq!(
            catalog
                .export_job(job_id)
                .expect("job")
                .expect("persisted job")
                .state,
            ExportJobState::Queued
        );
        let progress = catalog.export_job_progress(job_id).expect("progress");
        assert_eq!(progress.queued_item_count, 1);
        assert_eq!(progress.total_item_count, 1);
    }
}
