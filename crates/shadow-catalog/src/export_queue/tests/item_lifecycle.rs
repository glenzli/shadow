use crate::{
    AdvanceExportItem, Catalog, CatalogError, EnqueueExportJob, ExportFailure, ExportItemState,
    ExportJobState, ExportQueueRecovery, ExportSettingsSource, NewExportOutputReceipt,
};

use super::queue_fixtures::{SNAPSHOT_DIGEST, enqueue, seeded_item};

#[test]
fn queued_item_freezes_recipe_and_requires_receipt_before_completion() {
    let mut catalog = Catalog::open_in_memory().expect("catalog");
    let job = enqueue(&mut catalog);
    let claimed = catalog
        .claim_next_export_item(4)
        .expect("claim")
        .expect("one item");
    assert_eq!(claimed.state, ExportItemState::Preparing);
    assert_eq!(claimed.attempt_count, 1);
    assert_eq!(job.state, ExportJobState::Queued);

    let err = catalog
        .advance_export_item(&AdvanceExportItem {
            item_id: claimed.id,
            expected_state: ExportItemState::Preparing,
            next_state: ExportItemState::Completed,
            failure: None,
            receipt: None,
            now_ms: 5,
        })
        .expect_err("cannot skip rendering or completion receipt");
    assert!(matches!(err, CatalogError::InvalidExportTransition { .. }));

    let rendering = catalog
        .advance_export_item(&AdvanceExportItem {
            item_id: claimed.id,
            expected_state: ExportItemState::Preparing,
            next_state: ExportItemState::Rendering,
            failure: None,
            receipt: None,
            now_ms: 5,
        })
        .expect("rendering");
    let encoding = catalog
        .advance_export_item(&AdvanceExportItem {
            item_id: rendering.id,
            expected_state: ExportItemState::Rendering,
            next_state: ExportItemState::Encoding,
            failure: None,
            receipt: None,
            now_ms: 6,
        })
        .expect("encoding");
    let writing = catalog
        .advance_export_item(&AdvanceExportItem {
            item_id: encoding.id,
            expected_state: ExportItemState::Encoding,
            next_state: ExportItemState::WritingTemp,
            failure: None,
            receipt: None,
            now_ms: 7,
        })
        .expect("writing");
    let completed = catalog
        .advance_export_item(&AdvanceExportItem {
            item_id: writing.id,
            expected_state: ExportItemState::WritingTemp,
            next_state: ExportItemState::Completed,
            failure: None,
            receipt: Some(NewExportOutputReceipt {
                output: writing.output.clone(),
                output_format: "jpeg".into(),
                byte_len: 123,
                content_digest: Some([8; 32]),
                receipt_json: r#"{"color_space":"srgb"}"#.into(),
            }),
            now_ms: 8,
        })
        .expect("complete item");
    assert_eq!(completed.state, ExportItemState::Completed);
    assert!(
        catalog
            .export_output_receipt(completed.id)
            .expect("receipt query")
            .is_some()
    );
    assert_eq!(
        catalog.export_job(job.id).expect("job").expect("job").state,
        ExportJobState::Completed
    );
}

#[test]
fn failed_and_interrupted_items_can_be_retried_without_losing_snapshot() {
    let mut catalog = Catalog::open_in_memory().expect("catalog");
    let job = enqueue(&mut catalog);
    let claimed = catalog
        .claim_next_export_item(4)
        .expect("claim")
        .expect("item");
    let failed = catalog
        .advance_export_item(&AdvanceExportItem {
            item_id: claimed.id,
            expected_state: ExportItemState::Preparing,
            next_state: ExportItemState::Failed,
            failure: Some(ExportFailure {
                code: "encoder_failed".into(),
                message: "jpeg encoder rejected the temporary file".into(),
                retryable: true,
            }),
            receipt: None,
            now_ms: 5,
        })
        .expect("record failure");
    assert_eq!(failed.state, ExportItemState::Failed);
    assert_eq!(
        catalog.export_job(job.id).expect("job").expect("job").state,
        ExportJobState::Failed
    );

    let requeued = catalog
        .advance_export_item(&AdvanceExportItem {
            item_id: failed.id,
            expected_state: ExportItemState::Failed,
            next_state: ExportItemState::Queued,
            failure: None,
            receipt: None,
            now_ms: 6,
        })
        .expect("retry queue");
    assert_eq!(requeued.recipe_snapshot_digest, SNAPSHOT_DIGEST);
    assert!(requeued.error.is_none());
    assert!(requeued.started_at_ms.is_none());

    let preparing = catalog
        .claim_next_export_item(7)
        .expect("reclaim")
        .expect("item");
    assert_eq!(preparing.attempt_count, 2);
    let recovery = catalog
        .recover_and_requeue_interrupted_export_items(8)
        .expect("recover");
    assert_eq!(
        recovery,
        ExportQueueRecovery {
            interrupted_item_count: 1,
            requeued_item_count: 1,
            queued_item_count: 1,
        }
    );
    let queued = catalog
        .export_job_items(job.id)
        .expect("items")
        .pop()
        .expect("item");
    assert_eq!(queued.state, ExportItemState::Queued);
    assert_eq!(queued.recipe_commit_id, preparing.recipe_commit_id);
    assert_eq!(
        queued.recipe_snapshot_digest,
        preparing.recipe_snapshot_digest
    );
    assert_eq!(
        queued.source_identity_digest,
        preparing.source_identity_digest
    );
    assert_eq!(queued.render_plan_digest, preparing.render_plan_digest);
    assert_eq!(queued.output, preparing.output);
    assert_eq!(queued.attempt_count, preparing.attempt_count);
    assert!(queued.started_at_ms.is_none());
    assert!(queued.finished_at_ms.is_none());
    assert!(queued.error.is_none());
    assert_eq!(
        catalog.export_job(job.id).expect("job").expect("job").state,
        ExportJobState::Queued
    );
    assert!(
        catalog
            .export_job(job.id)
            .expect("job")
            .expect("job")
            .started_at_ms
            .is_none()
    );
}

#[test]
fn enqueue_rejects_a_recipe_digest_that_does_not_match_the_immutable_commit() {
    let mut catalog = Catalog::open_in_memory().expect("catalog");
    let mut item = seeded_item(&mut catalog);
    item.recipe_snapshot_digest = [9; 32];
    let error = catalog
        .enqueue_export_job(&EnqueueExportJob {
            settings: ExportSettingsSource::InlineJson(r#"{"format":"jpeg"}"#.into()),
            items: vec![item],
            now_ms: 3,
        })
        .expect_err("mismatched snapshot must fail");
    assert!(matches!(
        error,
        CatalogError::ExportRecipeSnapshotMismatch { .. }
    ));
}
