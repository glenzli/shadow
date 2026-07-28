use std::{
    path::Path,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
    thread,
    time::{Duration, Instant},
};

use shadow_catalog::CatalogActor;

use super::super::{
    DecodeInspectionError, DecodeInspectionPool, DecodeInspectionRequest,
    MAX_RECOMMENDED_INSPECTION_WORKERS, MIN_RECOMMENDED_INSPECTION_WORKERS, fingerprint_source,
    recommended_decode_inspection_worker_count,
};
use super::support::{Fixture, sample_snapshot};

#[test]
fn pool_runs_independent_inspectors_concurrently_and_merges_performance() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let (entered_sender, entered_receiver) = mpsc::channel();
    let (release_zero_sender, release_zero_receiver) = mpsc::sync_channel(0);
    let (release_one_sender, release_one_receiver) = mpsc::sync_channel(0);
    let mut releases = [Some(release_zero_receiver), Some(release_one_receiver)];
    let active = Arc::new(AtomicUsize::new(0));
    let maximum_active = Arc::new(AtomicUsize::new(0));
    let pool = DecodeInspectionPool::spawn_inner(
        &catalog,
        2,
        |worker_index| {
            let entered_sender = entered_sender.clone();
            let release = releases[worker_index]
                .take()
                .expect("one release gate per inspector");
            let active = Arc::clone(&active);
            let maximum_active = Arc::clone(&maximum_active);
            Ok::<_, String>(move |_path: &Path| {
                let now_active = active.fetch_add(1, Ordering::SeqCst) + 1;
                maximum_active.fetch_max(now_active, Ordering::SeqCst);
                entered_sender
                    .send(worker_index)
                    .expect("report pool worker entry");
                release.recv().expect("release pool worker");
                active.fetch_sub(1, Ordering::SeqCst);
                Ok(sample_snapshot())
            })
        },
        None,
        None,
        true,
        2,
    )
    .expect("spawn two-worker pool");
    let request = DecodeInspectionRequest {
        representation_id: registered.representation_id,
        path: fixture.raw_path.clone(),
        expected_source: source,
    };
    let handle = pool.handle();
    let first = handle
        .submit(request.clone())
        .expect("submit first pool job");
    let second = handle.submit(request).expect("submit second pool job");

    let first_worker = entered_receiver
        .recv_timeout(Duration::from_secs(1))
        .expect("first worker enters");
    let second_worker = entered_receiver
        .recv_timeout(Duration::from_secs(1))
        .expect("second worker enters concurrently");
    assert_ne!(first_worker, second_worker);
    release_zero_sender.send(()).expect("release worker zero");
    release_one_sender.send(()).expect("release worker one");
    first.wait().expect("first pool job completes");
    second.wait().expect("second pool job completes");

    let terminal = pool
        .shutdown_with_performance()
        .expect("drain pool with aggregate performance");
    assert_eq!(terminal.summary.completed, 2);
    assert_eq!(terminal.decode.provider_inspect.samples, 2);
    assert_eq!(maximum_active.load(Ordering::SeqCst), 2);
    assert_eq!(
        catalog
            .decode_snapshots(registered.representation_id)
            .expect("read pool-written snapshot")
            .len(),
        1
    );
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn pool_factory_failure_starts_no_reduced_capacity_worker_set() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let provider_calls = Arc::new(AtomicUsize::new(0));
    let factory_calls = Arc::new(AtomicUsize::new(0));
    let result = DecodeInspectionPool::spawn(actor.handle(), 2, {
        let provider_calls = Arc::clone(&provider_calls);
        let factory_calls = Arc::clone(&factory_calls);
        move |worker_index| {
            factory_calls.fetch_add(1, Ordering::SeqCst);
            if worker_index == 1 {
                return Err("fixture factory failure");
            }
            let provider_calls = Arc::clone(&provider_calls);
            Ok(move |_path: &Path| {
                provider_calls.fetch_add(1, Ordering::SeqCst);
                Ok(sample_snapshot())
            })
        }
    });
    assert!(matches!(
        result,
        Err(DecodeInspectionError::WorkerFactory {
            worker_index: 1,
            ..
        })
    ));
    assert_eq!(factory_calls.load(Ordering::SeqCst), 2);
    assert_eq!(provider_calls.load(Ordering::SeqCst), 0);
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn pool_provider_panic_closes_the_whole_submission_gate() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let source = fingerprint_source(&fixture.raw_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&fixture.registration(source))
        .expect("register source");
    let pool = DecodeInspectionPool::spawn(catalog, 2, |worker_index| {
        Ok::<_, String>(move |_path: &Path| {
            assert_ne!(worker_index, 0, "intentional pool provider panic");
            Ok(sample_snapshot())
        })
    })
    .expect("spawn panic fixture pool");
    let handle = pool.handle();
    let request = DecodeInspectionRequest {
        representation_id: registered.representation_id,
        path: fixture.raw_path.clone(),
        expected_source: source,
    };
    assert!(matches!(
        handle
            .submit(request.clone())
            .expect("submit panic fixture")
            .wait(),
        Err(DecodeInspectionError::WorkerUnavailable)
    ));
    let deadline = Instant::now() + Duration::from_secs(1);
    loop {
        if matches!(
            handle.submit(request.clone()),
            Err(DecodeInspectionError::WorkerUnavailable)
        ) {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "pool did not close after provider panic"
        );
        thread::yield_now();
    }
    assert!(matches!(
        pool.shutdown_with_summary(),
        Err(DecodeInspectionError::WorkerPanicked)
    ));
    actor.shutdown().expect("shutdown catalog");
}

#[test]
fn recommended_pool_size_is_explicitly_memory_bounded() {
    assert!(
        (MIN_RECOMMENDED_INSPECTION_WORKERS..=MAX_RECOMMENDED_INSPECTION_WORKERS)
            .contains(&recommended_decode_inspection_worker_count())
    );
}
