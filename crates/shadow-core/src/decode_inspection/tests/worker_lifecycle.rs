use std::{
    path::Path,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
    thread,
};

use shadow_catalog::CatalogActor;
use shadow_domain::{DecoderSnapshot, PreviewPayload};

use crate::{
    import::ScanCancellation,
    performance::{DecodePerformance, TechnicalPerformance},
};

use super::super::{
    DecodeInspectionActor, DecodeInspectionDiscardReason, DecodeInspectionError,
    DecodeInspectionOutcome, DecodeInspectionRequest, DecodeInspectionSummary, DecodeInspector,
    PreviewCacheOutcome, fingerprint_source,
};
use super::support::{Fixture, sample_snapshot};

#[test]
fn worker_persists_snapshot_without_decoding_on_caller_or_writer_thread() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let (thread_sender, thread_receiver) = mpsc::channel();
    let worker = DecodeInspectionActor::spawn(catalog.clone(), move |_path: &Path| {
        thread_sender
            .send(thread::current().name().map(str::to_owned))
            .expect("report worker thread");
        Ok(sample_snapshot())
    })
    .expect("spawn inspector");

    let request = DecodeInspectionRequest {
        representation_id: registered.representation_id,
        path: fixture.raw_path.clone(),
        expected_source: source,
    };
    let first_ticket = worker
        .handle()
        .submit(request.clone())
        .expect("submit first inspection");
    let second_ticket = worker
        .handle()
        .submit(request)
        .expect("submit second inspection");
    assert_eq!(
        second_ticket
            .wait_timeout(std::time::Duration::from_secs(1))
            .expect("complete second inspection without waiting for first ticket"),
        DecodeInspectionOutcome::Recorded {
            provider_id: "anonymous".into(),
            provider_version: "1".into(),
            preview: PreviewCacheOutcome::NotRequested,
        }
    );
    assert!(matches!(
        first_ticket.wait().expect("complete first inspection"),
        DecodeInspectionOutcome::Recorded { .. }
    ));
    for _ in 0..2 {
        assert_eq!(
            thread_receiver.recv().expect("worker name").as_deref(),
            Some("shadow-decode-inspector")
        );
    }
    let snapshots = catalog
        .decode_snapshots(registered.representation_id)
        .expect("read snapshot");
    assert_eq!(snapshots.len(), 1);
    assert_eq!(snapshots[0].snapshot, sample_snapshot());

    let terminal = worker
        .shutdown_with_performance()
        .expect("shutdown default inspector with terminal profile");
    assert_eq!(
        terminal.summary,
        DecodeInspectionSummary {
            completed: 2,
            hard_failures: 0,
            preview_failures: 0,
            cancelled: 0,
        }
    );
    assert!(!terminal.decode.profiled);
    assert_eq!(terminal.decode, DecodePerformance::default());
    assert_eq!(terminal.technical, TechnicalPerformance::default());
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn shutdown_summary_accounts_for_success_failures_and_local_cancellation() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let worker = DecodeInspectionActor::spawn_with_cache(
        catalog.clone(),
        SummaryInspector::default(),
        fixture.root.join("cache"),
    )
    .expect("spawn summary inspector");
    let handle = worker.handle();
    let request = DecodeInspectionRequest {
        representation_id: registered.representation_id,
        path: fixture.raw_path.clone(),
        expected_source: source,
    };

    let success = handle
        .submit(request.clone())
        .expect("submit successful inspection");
    let hard_failure = handle
        .submit(request.clone())
        .expect("submit hard-failing inspection");
    let preview_failure = handle
        .submit(request.clone())
        .expect("submit preview-failing inspection");
    let cancelled_token = ScanCancellation::new();
    cancelled_token.cancel();
    let cancelled = handle
        .submit_with_cancellation(request, &cancelled_token)
        .expect("accept locally cancelled inspection");

    assert!(matches!(
        success.wait().expect("complete successful inspection"),
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::NoVisualAvailable,
            ..
        }
    ));
    assert!(matches!(
        hard_failure.wait(),
        Err(DecodeInspectionError::Inspector { .. })
    ));
    assert!(matches!(
        preview_failure
            .wait()
            .expect("complete preview-failing inspection"),
        DecodeInspectionOutcome::Recorded {
            preview: PreviewCacheOutcome::Failed(_),
            ..
        }
    ));
    assert_eq!(
        cancelled.wait().expect("complete cancelled inspection"),
        DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
    );

    assert_eq!(
        worker
            .shutdown_with_summary()
            .expect("shutdown inspector with summary"),
        DecodeInspectionSummary {
            completed: 4,
            hard_failures: 1,
            preview_failures: 1,
            cancelled: 1,
        }
    );
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn submission_gate_rejects_a_concurrent_submit_at_the_shutdown_boundary() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let calls = Arc::new(AtomicUsize::new(0));
    let inspector_calls = Arc::clone(&calls);
    let worker = DecodeInspectionActor::spawn(catalog, move |_path: &Path| {
        inspector_calls.fetch_add(1, Ordering::SeqCst);
        Ok(sample_snapshot())
    })
    .expect("spawn inspector");
    let request = DecodeInspectionRequest {
        representation_id: registered.representation_id,
        path: fixture.raw_path.clone(),
        expected_source: source,
    };
    let state = Arc::clone(&worker.handle.state);
    let mut closing_gate = state.lock().expect("hold submission gate");
    let handle = worker.handle();
    let (started_sender, started_receiver) = mpsc::sync_channel(0);
    let submit_thread = thread::spawn(move || {
        started_sender.send(()).expect("announce concurrent submit");
        handle.submit(request)
    });
    started_receiver
        .recv()
        .expect("concurrent submit reaches gate");

    closing_gate.stopping = true;
    drop(closing_gate);
    assert!(matches!(
        submit_thread.join().expect("join concurrent submit"),
        Err(DecodeInspectionError::WorkerUnavailable)
    ));
    assert_eq!(calls.load(Ordering::SeqCst), 0);
    assert_eq!(
        worker
            .shutdown_with_summary()
            .expect("shutdown closed inspector"),
        DecodeInspectionSummary::default()
    );
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn profiled_submission_counts_full_queue_retries_without_timing_thresholds() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let (entered_sender, entered_receiver) = mpsc::sync_channel(0);
    let (release_sender, release_receiver) = mpsc::sync_channel(0);
    let mut first_job = true;
    let worker = DecodeInspectionActor::spawn_inner_with_capacity(
        &catalog,
        move |_path: &Path| {
            if first_job {
                first_job = false;
                entered_sender.send(()).expect("announce first job");
                release_receiver.recv().expect("release first job");
            }
            Ok(sample_snapshot())
        },
        None,
        true,
        1,
    )
    .expect("spawn one-slot profiled worker");
    let request = DecodeInspectionRequest {
        representation_id: registered.representation_id,
        path: fixture.raw_path.clone(),
        expected_source: source,
    };
    let handle = worker.handle();
    let first = handle.submit(request.clone()).expect("submit active job");
    entered_receiver.recv().expect("first job enters provider");
    let second = handle
        .submit(request.clone())
        .expect("fill the bounded queue");

    let third_handle = handle.clone();
    let (full_sender, full_receiver) = mpsc::sync_channel(0);
    let third = thread::spawn(move || {
        let mut announced = false;
        third_handle.submit_with_cancellation_inner(request, &ScanCancellation::new(), true, || {
            if !announced {
                full_sender.send(()).expect("announce full queue");
                announced = true;
            }
        })
    });
    full_receiver
        .recv()
        .expect("third submit deterministically observes a full queue");
    release_sender.send(()).expect("release active job");
    let third = third
        .join()
        .expect("join third submit")
        .expect("submit after queue frees");

    assert!(third.queue_full_events >= 1);
    first.wait().expect("finish first job");
    second.wait().expect("finish second job");
    third.ticket.wait().expect("finish third job");
    let terminal = worker
        .shutdown_with_performance()
        .expect("shutdown profiled worker");
    assert_eq!(terminal.summary.completed, 3);
    assert_eq!(terminal.decode.queue_wait.samples, 3);

    actor.shutdown().expect("shutdown catalog");
}

#[derive(Debug, Default)]
struct SummaryInspector {
    inspect_calls: usize,
    preview_calls: usize,
}

impl DecodeInspector for SummaryInspector {
    fn inspect(&mut self, _path: &Path) -> Result<DecoderSnapshot, String> {
        self.inspect_calls += 1;
        if self.inspect_calls == 2 {
            Err("hard inspection fixture".into())
        } else {
            Ok(sample_snapshot())
        }
    }

    fn extract_best_preview(&mut self, _path: &Path) -> Result<Option<PreviewPayload>, String> {
        self.preview_calls += 1;
        if self.preview_calls == 2 {
            Err("preview failure fixture".into())
        } else {
            Ok(None)
        }
    }
}
