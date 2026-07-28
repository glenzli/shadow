use std::{
    fs,
    path::Path,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
};

use shadow_catalog::CatalogActor;

use crate::import::ScanCancellation;

use super::super::{
    DecodeInspectionActor, DecodeInspectionDiscardReason, DecodeInspectionOutcome,
    DecodeInspectionRequest, DecodeInspectionSummary, fingerprint_source,
};
use super::support::{Fixture, sample_snapshot};

#[test]
fn changed_file_is_discarded_before_provider_work() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    fs::write(&fixture.raw_path, b"changed and longer").expect("change source");
    let worker = DecodeInspectionActor::spawn(catalog.clone(), |_path: &Path| {
        panic!("provider must not run for a stale source")
    })
    .expect("spawn inspector");

    let outcome = worker
        .handle()
        .submit(DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: fixture.raw_path.clone(),
            expected_source: source,
        })
        .expect("submit inspection")
        .wait()
        .expect("complete inspection");
    assert_eq!(
        outcome,
        DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::FilesystemChanged)
    );
    assert!(
        catalog
            .decode_snapshots(registered.representation_id)
            .expect("read snapshots")
            .is_empty()
    );

    worker.shutdown().expect("shutdown inspector");
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn queued_cancelled_inspections_never_begin_provider_work() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let calls = Arc::new(AtomicUsize::new(0));
    let inspector_calls = Arc::clone(&calls);
    let (entered_sender, entered_receiver) = mpsc::sync_channel(0);
    let (release_sender, release_receiver) = mpsc::sync_channel(0);
    let worker = DecodeInspectionActor::spawn(catalog.clone(), move |_path: &Path| {
        inspector_calls.fetch_add(1, Ordering::SeqCst);
        entered_sender.send(()).expect("announce provider entry");
        release_receiver.recv().expect("release provider");
        Ok(sample_snapshot())
    })
    .expect("spawn inspector");
    let request = DecodeInspectionRequest {
        representation_id: registered.representation_id,
        path: fixture.raw_path.clone(),
        expected_source: source,
    };
    let cancellation = ScanCancellation::new();
    let first = worker
        .handle()
        .submit_with_cancellation(request.clone(), &cancellation)
        .expect("submit active inspection");
    entered_receiver.recv().expect("provider starts first job");
    let queued = (0..3)
        .map(|_| {
            worker
                .handle()
                .submit_with_cancellation(request.clone(), &cancellation)
                .expect("submit queued inspection")
        })
        .collect::<Vec<_>>();

    cancellation.cancel();
    release_sender.send(()).expect("release active inspection");
    assert_eq!(
        first.wait().expect("active job observes cancellation"),
        DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
    );
    for ticket in queued {
        assert_eq!(
            ticket.wait().expect("queued job is discarded"),
            DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
        );
    }
    assert_eq!(calls.load(Ordering::SeqCst), 1);
    assert!(
        catalog
            .decode_snapshots(registered.representation_id)
            .expect("read snapshots")
            .is_empty()
    );

    assert_eq!(
        worker
            .shutdown_with_summary()
            .expect("shutdown inspector with cancellation summary"),
        DecodeInspectionSummary {
            completed: 4,
            hard_failures: 0,
            preview_failures: 0,
            cancelled: 4,
        }
    );
    actor.shutdown().expect("shutdown catalog");
}
