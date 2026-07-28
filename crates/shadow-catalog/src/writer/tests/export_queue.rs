use shadow_domain::{
    AssetLocation, EntityId, Platform, RecipeCommit, RecipeCommitId, RecipeId, RecipeSnapshot,
    RepresentationKind,
};

use crate::{
    AdvanceExportItem, CatalogError, CommitRecipe, EnqueueExportJob, ExportItemRecord,
    ExportItemState, ExportJobRecord, ExportJobState, ExportQueueRecovery, ExportSettingsSource,
    NewExportItem, NewExportOutputReceipt, RegisterAsset,
};

use crate::writer::{CatalogActor, CatalogHandle};

fn enqueue_one(handle: &CatalogHandle, suffix: &str, now_ms: i64) -> ExportJobRecord {
    let source_path = format!("/photos/export-{suffix}.nef");
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                source_path.as_bytes().to_vec(),
                &source_path,
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms,
        })
        .expect("register export source");
    let commit = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        RecipeSnapshot::empty(),
        Some(format!("Export actor {suffix}")),
        now_ms + 1,
    )
    .expect("build export Recipe");
    let recipe = handle
        .commit_recipe(&CommitRecipe {
            photo_id: registered.photo_id,
            commit,
            update_refs: Vec::new(),
        })
        .expect("commit export Recipe");
    let output_path = format!("/exports/export-{suffix}.jpg");
    handle
        .enqueue_export_job(&EnqueueExportJob {
            settings: ExportSettingsSource::InlineJson(r#"{"format":"jpeg","quality":90}"#.into()),
            items: vec![NewExportItem {
                photo_id: registered.photo_id,
                representation_id: registered.representation_id,
                recipe_commit_id: recipe.commit.id(),
                recipe_snapshot_digest: recipe.snapshot_digest,
                edit_commit_id: None,
                source_identity_json: r#"{"scope":"whole_file","digest":"actor"}"#.into(),
                render_plan_json: r#"{"quality":"full","provider":"actor"}"#.into(),
                output: AssetLocation::new(
                    Platform::MacOs,
                    output_path.as_bytes().to_vec(),
                    output_path,
                ),
            }],
            now_ms: now_ms + 2,
        })
        .expect("enqueue export through actor")
}

fn advance(
    handle: &CatalogHandle,
    item: &ExportItemRecord,
    next_state: ExportItemState,
    now_ms: i64,
) -> ExportItemRecord {
    handle
        .advance_export_item(&AdvanceExportItem {
            item_id: item.id,
            expected_state: item.state,
            next_state,
            failure: None,
            receipt: None,
            now_ms,
        })
        .expect("advance export item through actor")
}

#[test]
#[allow(clippy::too_many_lines)]
fn actor_routes_a_complete_export_item_lifecycle() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let job = enqueue_one(&handle, "complete", 1);
    assert_eq!(job.state, ExportJobState::Queued);
    assert_eq!(
        handle.export_job(job.id).expect("read job"),
        Some(job.clone())
    );
    assert_eq!(handle.export_jobs(1).expect("list jobs"), vec![job.clone()]);

    let queued = handle
        .export_job_items(job.id)
        .expect("list job items")
        .pop()
        .expect("one queued item");
    assert_eq!(queued.state, ExportItemState::Queued);
    assert_eq!(queued.position, 0);
    assert_eq!(
        handle.export_item(queued.id).expect("read exact item"),
        Some(queued.clone())
    );
    let queued_progress = handle
        .export_job_progress(job.id)
        .expect("read queued progress");
    assert_eq!(queued_progress.total_item_count, 1);
    assert_eq!(queued_progress.queued_item_count, 1);

    let preparing = handle
        .claim_next_export_item(4)
        .expect("claim queued item")
        .expect("one claimed item");
    assert_eq!(preparing.id, queued.id);
    assert_eq!(preparing.state, ExportItemState::Preparing);
    assert_eq!(preparing.attempt_count, 1);
    assert_eq!(
        handle
            .export_job(job.id)
            .expect("read running job")
            .expect("job exists")
            .state,
        ExportJobState::Running
    );

    let rendering = advance(&handle, &preparing, ExportItemState::Rendering, 5);
    let encoding = advance(&handle, &rendering, ExportItemState::Encoding, 6);
    let writing = advance(&handle, &encoding, ExportItemState::WritingTemp, 7);
    assert!(matches!(
        handle.advance_export_item(&AdvanceExportItem {
            item_id: writing.id,
            expected_state: ExportItemState::WritingTemp,
            next_state: ExportItemState::Completed,
            failure: None,
            receipt: None,
            now_ms: 8,
        }),
        Err(CatalogError::InvalidExport(_))
    ));
    assert_eq!(
        handle
            .export_item(writing.id)
            .expect("read unchanged writing item")
            .expect("item exists")
            .state,
        ExportItemState::WritingTemp
    );

    let completed = handle
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
            now_ms: 9,
        })
        .expect("complete item through actor");
    assert_eq!(completed.state, ExportItemState::Completed);
    let receipt = handle
        .export_output_receipt(completed.id)
        .expect("read output receipt")
        .expect("receipt exists");
    assert_eq!(receipt.item_id, completed.id);
    assert_eq!(receipt.output, completed.output);
    assert_eq!(receipt.output_format, "jpeg");
    assert_eq!(receipt.byte_len, 123);
    assert_eq!(receipt.content_digest, Some([8; 32]));
    let completed_job = handle
        .export_job(job.id)
        .expect("read completed job")
        .expect("job exists");
    assert_eq!(completed_job.state, ExportJobState::Completed);
    assert_eq!(completed_job.finished_at_ms, Some(9));
    let completed_progress = handle
        .export_job_progress(job.id)
        .expect("read completed progress");
    assert_eq!(completed_progress.total_item_count, 1);
    assert_eq!(completed_progress.completed_item_count, 1);
    assert_eq!(completed_progress.active_item_count(), 0);
    assert_eq!(completed_progress.resumable_item_count(), 0);
    assert!(
        handle
            .claim_next_export_item(10)
            .expect("queue is empty")
            .is_none()
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_routes_export_recovery_and_cancellation() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let job = enqueue_one(&handle, "recover", 20);
    let first_claim = handle
        .claim_next_export_item(23)
        .expect("claim export")
        .expect("claimed item");

    assert_eq!(
        handle
            .recover_interrupted_export_jobs(24)
            .expect("legacy recovery through actor"),
        1
    );
    let queued = handle
        .export_item(first_claim.id)
        .expect("read recovered item")
        .expect("item exists");
    assert_eq!(queued.state, ExportItemState::Queued);
    let second_claim = handle
        .claim_next_export_item(25)
        .expect("reclaim export")
        .expect("claimed item again");
    assert_eq!(second_claim.attempt_count, 2);
    assert_eq!(
        handle
            .recover_and_requeue_interrupted_export_items(26)
            .expect("complete recovery through actor"),
        ExportQueueRecovery {
            interrupted_item_count: 1,
            requeued_item_count: 1,
            queued_item_count: 1,
        }
    );

    let cancelled = handle
        .cancel_export_job(job.id, 27)
        .expect("cancel export through actor");
    assert_eq!(cancelled.state, ExportJobState::Cancelled);
    let progress = handle
        .export_job_progress(job.id)
        .expect("read cancelled progress");
    assert_eq!(progress.cancelled_item_count, 1);
    assert_eq!(progress.total_item_count, 1);
    actor.shutdown().expect("shutdown actor");
}
