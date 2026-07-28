use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread,
    time::Duration,
};

use super::{
    WorkDiscardReason, WorkGenerationSource, WorkOutcome, WorkPriority, WorkScheduler,
    WorkSchedulerConfig, WorkSpec, WorkSubmitError,
};

#[test]
fn queued_work_runs_by_priority_then_fifo() {
    let scheduler = WorkScheduler::start(&WorkSchedulerConfig::new(1, 4)).expect("scheduler");
    let handle = scheduler.handle();
    let (entered_sender, entered_receiver) = mpsc::sync_channel(0);
    let (release_sender, release_receiver) = mpsc::sync_channel(0);
    let blocker = handle
        .try_submit(WorkSpec::new(WorkPriority::CurrentInteraction), move |_| {
            entered_sender.send(()).expect("report blocker");
            release_receiver.recv().expect("release blocker");
        })
        .expect("submit blocker");
    entered_receiver.recv().expect("blocker entered");

    let order = Arc::new(Mutex::new(Vec::new()));
    let background_order = Arc::clone(&order);
    let background = handle
        .try_submit(WorkSpec::new(WorkPriority::BatchExport), move |_| {
            background_order
                .lock()
                .expect("background order")
                .push("background");
        })
        .expect("submit background");
    let viewport_order = Arc::clone(&order);
    let viewport = handle
        .try_submit(WorkSpec::new(WorkPriority::CurrentViewport), move |_| {
            viewport_order
                .lock()
                .expect("viewport order")
                .push("viewport");
        })
        .expect("submit viewport");

    release_sender.send(()).expect("release blocker");
    assert_eq!(
        blocker.wait().expect("blocker outcome"),
        WorkOutcome::Completed(())
    );
    assert_eq!(
        viewport.wait().expect("viewport outcome"),
        WorkOutcome::Completed(())
    );
    assert_eq!(
        background.wait().expect("background outcome"),
        WorkOutcome::Completed(())
    );
    assert_eq!(
        *order.lock().expect("final order"),
        ["viewport", "background"]
    );
    scheduler.shutdown().expect("shutdown");
}

#[test]
fn queued_cancellation_skips_the_closure() {
    let scheduler = WorkScheduler::start(&WorkSchedulerConfig::new(1, 2)).expect("scheduler");
    let handle = scheduler.handle();
    let (entered_sender, entered_receiver) = mpsc::sync_channel(0);
    let (release_sender, release_receiver) = mpsc::sync_channel(0);
    let blocker = handle
        .try_submit(WorkSpec::new(WorkPriority::CurrentInteraction), move |_| {
            entered_sender.send(()).expect("report blocker");
            release_receiver.recv().expect("release blocker");
        })
        .expect("submit blocker");
    entered_receiver.recv().expect("blocker entered");
    let called = Arc::new(AtomicBool::new(false));
    let called_by_job = Arc::clone(&called);
    let cancelled = handle
        .try_submit(WorkSpec::new(WorkPriority::VisibleThumbnail), move |_| {
            called_by_job.store(true, Ordering::Release);
        })
        .expect("submit cancellable");
    cancelled.cancel();
    release_sender.send(()).expect("release blocker");

    assert_eq!(
        blocker.wait().expect("blocker outcome"),
        WorkOutcome::Completed(())
    );
    assert_eq!(
        cancelled.wait().expect("cancelled outcome"),
        WorkOutcome::Discarded(WorkDiscardReason::Cancelled)
    );
    assert!(!called.load(Ordering::Acquire));
    scheduler.shutdown().expect("shutdown");
}

#[test]
fn superseded_generation_never_publishes_stale_output() {
    let scheduler = WorkScheduler::start(&WorkSchedulerConfig::new(1, 2)).expect("scheduler");
    let handle = scheduler.handle();
    let (entered_sender, entered_receiver) = mpsc::sync_channel(0);
    let (release_sender, release_receiver) = mpsc::sync_channel(0);
    let blocker = handle
        .try_submit(WorkSpec::new(WorkPriority::CurrentInteraction), move |_| {
            entered_sender.send(()).expect("report blocker");
            release_receiver.recv().expect("release blocker");
        })
        .expect("submit blocker");
    entered_receiver.recv().expect("blocker entered");

    let generations = WorkGenerationSource::new();
    let stale = handle
        .try_submit(
            WorkSpec::new(WorkPriority::CurrentViewport).in_generation(generations.current()),
            |_| 42,
        )
        .expect("submit generation");
    let _new_generation = generations.advance();
    release_sender.send(()).expect("release blocker");

    assert_eq!(
        blocker.wait().expect("blocker outcome"),
        WorkOutcome::Completed(())
    );
    assert_eq!(
        stale.wait().expect("stale outcome"),
        WorkOutcome::Discarded(WorkDiscardReason::SupersededGeneration)
    );
    scheduler.shutdown().expect("shutdown");
}

#[test]
fn bounded_queue_rejects_work_without_blocking_submitter() {
    let scheduler = WorkScheduler::start(&WorkSchedulerConfig::new(1, 1)).expect("scheduler");
    let handle = scheduler.handle();
    let (entered_sender, entered_receiver) = mpsc::sync_channel(0);
    let (release_sender, release_receiver) = mpsc::sync_channel(0);
    let blocker = handle
        .try_submit(WorkSpec::new(WorkPriority::CurrentInteraction), move |_| {
            entered_sender.send(()).expect("report blocker");
            release_receiver.recv().expect("release blocker");
        })
        .expect("submit blocker");
    entered_receiver.recv().expect("blocker entered");
    let queued = handle
        .try_submit(WorkSpec::new(WorkPriority::BatchExport), |_| ())
        .expect("fill queue");
    assert!(matches!(
        handle.try_submit(WorkSpec::new(WorkPriority::CurrentViewport), |_| ()),
        Err(WorkSubmitError::QueueFull)
    ));

    release_sender.send(()).expect("release blocker");
    let _ = blocker.wait().expect("blocker outcome");
    let _ = queued.wait().expect("queued outcome");
    scheduler.shutdown().expect("shutdown");
}

#[test]
fn task_panic_is_reported_and_worker_remains_available() {
    let scheduler = WorkScheduler::start(&WorkSchedulerConfig::new(1, 2)).expect("scheduler");
    let handle = scheduler.handle();
    let panicked = handle
        .try_submit(WorkSpec::new(WorkPriority::CurrentViewport), |_| -> () {
            panic!("test panic");
        })
        .expect("submit panic");
    let healthy = handle
        .try_submit(WorkSpec::new(WorkPriority::CurrentViewport), |_| 7)
        .expect("submit healthy");

    assert_eq!(
        panicked.wait().expect("panic outcome"),
        WorkOutcome::Panicked("test panic".to_owned())
    );
    assert_eq!(
        healthy.wait().expect("healthy outcome"),
        WorkOutcome::Completed(7)
    );
    let snapshot = handle.snapshot();
    assert_eq!(snapshot.panicked, 1);
    assert_eq!(snapshot.completed, 1);
    scheduler.shutdown().expect("shutdown");
}

#[test]
fn shutdown_discards_queued_work_and_rejects_new_submissions() {
    let scheduler = WorkScheduler::start(&WorkSchedulerConfig::new(1, 2)).expect("scheduler");
    let handle = scheduler.handle();
    let (entered_sender, entered_receiver) = mpsc::sync_channel(0);
    let (release_sender, release_receiver) = mpsc::sync_channel(0);
    let blocker = handle
        .try_submit(
            WorkSpec::new(WorkPriority::CurrentInteraction),
            move |context| {
                entered_sender.send(()).expect("report blocker");
                loop {
                    if context.is_cancelled() {
                        break;
                    }
                    if release_receiver
                        .recv_timeout(Duration::from_millis(5))
                        .is_ok()
                    {
                        break;
                    }
                }
            },
        )
        .expect("submit blocker");
    entered_receiver.recv().expect("blocker entered");
    let queued = handle
        .try_submit(WorkSpec::new(WorkPriority::BatchExport), |_| 9)
        .expect("submit queued");
    let shutdown_thread = thread::spawn(move || scheduler.shutdown());

    while handle.snapshot().accepting {
        thread::yield_now();
    }
    assert!(matches!(
        handle.try_submit(WorkSpec::new(WorkPriority::CurrentViewport), |_| ()),
        Err(WorkSubmitError::ShuttingDown)
    ));
    let _ = release_sender.send(());
    assert_eq!(
        blocker.wait().expect("blocker outcome"),
        WorkOutcome::Discarded(WorkDiscardReason::SchedulerShutdown)
    );
    assert_eq!(
        queued.wait().expect("queued outcome"),
        WorkOutcome::Discarded(WorkDiscardReason::SchedulerShutdown)
    );
    shutdown_thread
        .join()
        .expect("join shutdown")
        .expect("shutdown scheduler");
}
