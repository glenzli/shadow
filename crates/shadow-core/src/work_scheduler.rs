//! Priority and cancellation foundation shared by interactive and background work.
//!
//! This module owns scheduling mechanics, not image or Catalog semantics. Jobs
//! stay responsible for checking [`WorkContext`] at natural boundaries and for
//! publishing results transactionally only while their generation is current.

use std::{
    cmp::Ordering as Comparison,
    collections::BinaryHeap,
    fmt,
    panic::{self, AssertUnwindSafe},
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc::{self, Receiver, TryRecvError},
    },
    thread::{self, JoinHandle},
};

use thiserror::Error;

/// Product-level priority classes. Higher variants run before lower variants.
#[derive(Debug, Clone, Copy, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[repr(u8)]
pub enum WorkPriority {
    BatchExport = 0,
    BackgroundAnalysis = 1,
    ImportVerification = 2,
    NearbyPrefetch = 3,
    VisibleThumbnail = 4,
    CurrentViewport = 5,
    CurrentInteraction = 6,
}

/// Cooperative cancellation shared by a ticket and its executing job.
#[derive(Debug, Clone, Default)]
pub struct WorkCancellation {
    cancelled: Arc<AtomicBool>,
}

impl WorkCancellation {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }

    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }
}

/// Monotonic source used to supersede stale viewport or slider generations.
#[derive(Debug, Clone, Default)]
pub struct WorkGenerationSource {
    current: Arc<AtomicU64>,
}

impl WorkGenerationSource {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn current(&self) -> WorkGeneration {
        WorkGeneration {
            current: Arc::clone(&self.current),
            expected: self.current.load(Ordering::Acquire),
        }
    }

    /// Advances the source and returns the newly current generation.
    pub fn advance(&self) -> WorkGeneration {
        let expected = self
            .current
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |value| {
                Some(value.saturating_add(1))
            })
            .unwrap_or_else(|value| value)
            .saturating_add(1);
        WorkGeneration {
            current: Arc::clone(&self.current),
            expected,
        }
    }
}

/// A point-in-time generation guard attached to one scheduled job.
#[derive(Debug, Clone)]
pub struct WorkGeneration {
    current: Arc<AtomicU64>,
    expected: u64,
}

impl WorkGeneration {
    pub fn value(&self) -> u64 {
        self.expected
    }

    pub fn is_current(&self) -> bool {
        self.current.load(Ordering::Acquire) == self.expected
    }
}

/// Scheduling metadata for one job.
#[derive(Debug, Clone)]
pub struct WorkSpec {
    pub priority: WorkPriority,
    pub generation: Option<WorkGeneration>,
}

impl WorkSpec {
    pub const fn new(priority: WorkPriority) -> Self {
        Self {
            priority,
            generation: None,
        }
    }

    #[must_use]
    pub fn in_generation(mut self, generation: WorkGeneration) -> Self {
        self.generation = Some(generation);
        self
    }
}

/// Why a job result was deliberately discarded.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum WorkDiscardReason {
    Cancelled,
    SupersededGeneration,
    SchedulerShutdown,
}

/// Context passed to a scheduled closure for cooperative cancellation checks.
#[derive(Debug, Clone)]
pub struct WorkContext {
    id: u64,
    priority: WorkPriority,
    cancellation: WorkCancellation,
    generation: Option<WorkGeneration>,
    scheduler_shutdown: Arc<AtomicBool>,
}

impl WorkContext {
    pub const fn id(&self) -> u64 {
        self.id
    }

    pub const fn priority(&self) -> WorkPriority {
        self.priority
    }

    pub fn discard_reason(&self) -> Option<WorkDiscardReason> {
        if self.scheduler_shutdown.load(Ordering::Acquire) {
            Some(WorkDiscardReason::SchedulerShutdown)
        } else if self.cancellation.is_cancelled() {
            Some(WorkDiscardReason::Cancelled)
        } else if self
            .generation
            .as_ref()
            .is_some_and(|generation| !generation.is_current())
        {
            Some(WorkDiscardReason::SupersededGeneration)
        } else {
            None
        }
    }

    pub fn is_cancelled(&self) -> bool {
        self.discard_reason().is_some()
    }
}

/// Terminal result delivered to one typed ticket.
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum WorkOutcome<T> {
    Completed(T),
    Discarded(WorkDiscardReason),
    Panicked(String),
}

/// Scheduler construction failures.
#[derive(Debug, Error)]
pub enum WorkSchedulerError {
    #[error("work scheduler requires at least one worker")]
    NoWorkers,
    #[error("work scheduler queue capacity must be non-zero")]
    ZeroQueueCapacity,
    #[error("could not start work scheduler thread: {0}")]
    WorkerStart(#[source] std::io::Error),
    #[error("work scheduler worker panicked during shutdown")]
    WorkerPanicked,
}

/// Submission failures that leave the caller's job unqueued.
#[derive(Debug, Error, Clone, Copy, Eq, PartialEq)]
pub enum WorkSubmitError {
    #[error("work scheduler queue is full")]
    QueueFull,
    #[error("work scheduler is shutting down")]
    ShuttingDown,
}

/// Failure to receive a terminal result from a worker.
#[derive(Debug, Error, Clone, Copy, Eq, PartialEq)]
pub enum WorkWaitError {
    #[error("work scheduler disconnected before publishing a terminal result")]
    Disconnected,
}

/// Worker-pool and bounded-queue configuration.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct WorkSchedulerConfig {
    pub worker_count: usize,
    pub queue_capacity: usize,
    pub thread_name_prefix: String,
}

impl WorkSchedulerConfig {
    pub fn new(worker_count: usize, queue_capacity: usize) -> Self {
        Self {
            worker_count,
            queue_capacity,
            thread_name_prefix: "shadow-work".to_owned(),
        }
    }
}

/// Cheap diagnostics snapshot suitable for a task-center UI.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct WorkSchedulerSnapshot {
    pub accepting: bool,
    pub queued: usize,
    pub submitted: u64,
    pub started: u64,
    pub completed: u64,
    pub discarded: u64,
    pub panicked: u64,
}

/// Owns worker threads. Use its cloneable [`WorkSchedulerHandle`] to submit.
#[derive(Debug)]
pub struct WorkScheduler {
    handle: WorkSchedulerHandle,
    workers: Vec<JoinHandle<()>>,
}

/// Cloneable submission and diagnostics handle.
#[derive(Clone)]
pub struct WorkSchedulerHandle {
    shared: Arc<Shared>,
}

impl fmt::Debug for WorkSchedulerHandle {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("WorkSchedulerHandle")
            .field("snapshot", &self.snapshot())
            .finish()
    }
}

/// Typed completion handle for one scheduled job.
pub struct WorkTicket<T> {
    id: u64,
    cancellation: WorkCancellation,
    receiver: Receiver<WorkOutcome<T>>,
}

impl<T> fmt::Debug for WorkTicket<T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("WorkTicket")
            .field("id", &self.id)
            .field("cancelled", &self.cancellation.is_cancelled())
            .finish_non_exhaustive()
    }
}

impl<T> WorkTicket<T> {
    pub const fn id(&self) -> u64 {
        self.id
    }

    pub fn cancellation(&self) -> WorkCancellation {
        self.cancellation.clone()
    }

    pub fn cancel(&self) {
        self.cancellation.cancel();
    }

    /// Waits for one terminal result.
    ///
    /// # Errors
    ///
    /// Returns [`WorkWaitError::Disconnected`] if the scheduler disappears
    /// without publishing an outcome.
    pub fn wait(self) -> Result<WorkOutcome<T>, WorkWaitError> {
        self.receiver
            .recv()
            .map_err(|_| WorkWaitError::Disconnected)
    }

    /// Reads one terminal result without blocking.
    ///
    /// # Errors
    ///
    /// Returns [`WorkWaitError::Disconnected`] if the scheduler disappears
    /// without publishing an outcome.
    pub fn try_wait(&self) -> Result<Option<WorkOutcome<T>>, WorkWaitError> {
        match self.receiver.try_recv() {
            Ok(outcome) => Ok(Some(outcome)),
            Err(TryRecvError::Empty) => Ok(None),
            Err(TryRecvError::Disconnected) => Err(WorkWaitError::Disconnected),
        }
    }
}

struct Shared {
    state: Mutex<State>,
    ready: Condvar,
    shutdown: Arc<AtomicBool>,
    queue_capacity: usize,
    next_id: AtomicU64,
    next_sequence: AtomicU64,
    submitted: AtomicU64,
    started: AtomicU64,
    completed: AtomicU64,
    discarded: AtomicU64,
    panicked: AtomicU64,
}

struct State {
    accepting: bool,
    queue: BinaryHeap<QueuedWork>,
}

struct QueuedWork {
    id: u64,
    priority: WorkPriority,
    sequence: u64,
    execute: Box<dyn FnOnce() + Send + 'static>,
}

impl fmt::Debug for QueuedWork {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("QueuedWork")
            .field("id", &self.id)
            .field("priority", &self.priority)
            .field("sequence", &self.sequence)
            .finish_non_exhaustive()
    }
}

impl PartialEq for QueuedWork {
    fn eq(&self, other: &Self) -> bool {
        self.priority == other.priority && self.sequence == other.sequence
    }
}

impl Eq for QueuedWork {}

impl PartialOrd for QueuedWork {
    fn partial_cmp(&self, other: &Self) -> Option<Comparison> {
        Some(self.cmp(other))
    }
}

impl Ord for QueuedWork {
    fn cmp(&self, other: &Self) -> Comparison {
        self.priority
            .cmp(&other.priority)
            .then_with(|| other.sequence.cmp(&self.sequence))
    }
}

impl WorkScheduler {
    /// Starts a bounded worker pool.
    ///
    /// # Errors
    ///
    /// Returns a configuration error for zero workers/capacity, or
    /// [`WorkSchedulerError::WorkerStart`] if a worker thread cannot start.
    pub fn start(config: &WorkSchedulerConfig) -> Result<Self, WorkSchedulerError> {
        if config.worker_count == 0 {
            return Err(WorkSchedulerError::NoWorkers);
        }
        if config.queue_capacity == 0 {
            return Err(WorkSchedulerError::ZeroQueueCapacity);
        }
        let shared = Arc::new(Shared {
            state: Mutex::new(State {
                accepting: true,
                queue: BinaryHeap::new(),
            }),
            ready: Condvar::new(),
            shutdown: Arc::new(AtomicBool::new(false)),
            queue_capacity: config.queue_capacity,
            next_id: AtomicU64::new(0),
            next_sequence: AtomicU64::new(0),
            submitted: AtomicU64::new(0),
            started: AtomicU64::new(0),
            completed: AtomicU64::new(0),
            discarded: AtomicU64::new(0),
            panicked: AtomicU64::new(0),
        });
        let handle = WorkSchedulerHandle {
            shared: Arc::clone(&shared),
        };
        let mut workers = Vec::with_capacity(config.worker_count);
        for index in 0..config.worker_count {
            let worker_shared = Arc::clone(&shared);
            let thread_name = format!("{}-{index}", config.thread_name_prefix);
            match thread::Builder::new()
                .name(thread_name)
                .spawn(move || run_worker(&worker_shared))
            {
                Ok(worker) => workers.push(worker),
                Err(source) => {
                    stop_accepting(&shared);
                    for worker in workers {
                        let _ = worker.join();
                    }
                    return Err(WorkSchedulerError::WorkerStart(source));
                }
            }
        }
        Ok(Self { handle, workers })
    }

    pub fn handle(&self) -> WorkSchedulerHandle {
        self.handle.clone()
    }

    /// Cancels queued/active work cooperatively and joins every worker.
    ///
    /// # Errors
    ///
    /// Returns [`WorkSchedulerError::WorkerPanicked`] if a worker terminates
    /// outside the task-level panic boundary.
    pub fn shutdown(mut self) -> Result<(), WorkSchedulerError> {
        self.stop_and_join()
    }

    fn stop_and_join(&mut self) -> Result<(), WorkSchedulerError> {
        stop_accepting(&self.handle.shared);
        let mut panicked = false;
        for worker in self.workers.drain(..) {
            if worker.join().is_err() {
                panicked = true;
            }
        }
        if panicked {
            Err(WorkSchedulerError::WorkerPanicked)
        } else {
            Ok(())
        }
    }
}

impl Drop for WorkScheduler {
    fn drop(&mut self) {
        let _ = self.stop_and_join();
    }
}

impl WorkSchedulerHandle {
    /// Queues typed work without blocking the submitting thread.
    ///
    /// The closure is never called when cancellation, generation supersession,
    /// or shutdown is already visible before execution. After computation, the
    /// result is checked again and discarded when it became stale.
    ///
    /// # Errors
    ///
    /// Returns [`WorkSubmitError::QueueFull`] without blocking when the bounded
    /// queue is full, or [`WorkSubmitError::ShuttingDown`] after shutdown begins.
    pub fn try_submit<T, F>(
        &self,
        spec: WorkSpec,
        work: F,
    ) -> Result<WorkTicket<T>, WorkSubmitError>
    where
        T: Send + 'static,
        F: FnOnce(WorkContext) -> T + Send + 'static,
    {
        let id = self
            .shared
            .next_id
            .fetch_add(1, Ordering::Relaxed)
            .saturating_add(1);
        let sequence = self.shared.next_sequence.fetch_add(1, Ordering::Relaxed);
        let cancellation = WorkCancellation::new();
        let ticket_cancellation = cancellation.clone();
        let (sender, receiver) = mpsc::sync_channel(1);
        let shared = Arc::clone(&self.shared);
        let context = WorkContext {
            id,
            priority: spec.priority,
            cancellation,
            generation: spec.generation,
            scheduler_shutdown: Arc::clone(&shared.shutdown),
        };
        let execute = Box::new(move || {
            shared.started.fetch_add(1, Ordering::Relaxed);
            let outcome = if let Some(reason) = context.discard_reason() {
                shared.discarded.fetch_add(1, Ordering::Relaxed);
                WorkOutcome::Discarded(reason)
            } else {
                match panic::catch_unwind(AssertUnwindSafe(|| work(context.clone()))) {
                    Ok(value) => {
                        if let Some(reason) = context.discard_reason() {
                            shared.discarded.fetch_add(1, Ordering::Relaxed);
                            WorkOutcome::Discarded(reason)
                        } else {
                            shared.completed.fetch_add(1, Ordering::Relaxed);
                            WorkOutcome::Completed(value)
                        }
                    }
                    Err(payload) => {
                        shared.panicked.fetch_add(1, Ordering::Relaxed);
                        WorkOutcome::Panicked(panic_message(payload.as_ref()))
                    }
                }
            };
            let _ = sender.send(outcome);
        });
        let queued = QueuedWork {
            id,
            priority: spec.priority,
            sequence,
            execute,
        };

        {
            let mut state = self
                .shared
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if !state.accepting {
                return Err(WorkSubmitError::ShuttingDown);
            }
            if state.queue.len() >= self.shared.queue_capacity {
                return Err(WorkSubmitError::QueueFull);
            }
            state.queue.push(queued);
            self.shared.submitted.fetch_add(1, Ordering::Relaxed);
        }
        self.shared.ready.notify_one();
        Ok(WorkTicket {
            id,
            cancellation: ticket_cancellation,
            receiver,
        })
    }

    pub fn snapshot(&self) -> WorkSchedulerSnapshot {
        let state = self
            .shared
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        WorkSchedulerSnapshot {
            accepting: state.accepting,
            queued: state.queue.len(),
            submitted: self.shared.submitted.load(Ordering::Relaxed),
            started: self.shared.started.load(Ordering::Relaxed),
            completed: self.shared.completed.load(Ordering::Relaxed),
            discarded: self.shared.discarded.load(Ordering::Relaxed),
            panicked: self.shared.panicked.load(Ordering::Relaxed),
        }
    }
}

fn run_worker(shared: &Arc<Shared>) {
    loop {
        let work = {
            let mut state = shared
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            loop {
                if let Some(work) = state.queue.pop() {
                    break Some(work);
                }
                if !state.accepting {
                    break None;
                }
                state = shared
                    .ready
                    .wait(state)
                    .unwrap_or_else(std::sync::PoisonError::into_inner);
            }
        };
        let Some(work) = work else {
            return;
        };
        (work.execute)();
    }
}

fn stop_accepting(shared: &Arc<Shared>) {
    shared.shutdown.store(true, Ordering::Release);
    {
        let mut state = shared
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        state.accepting = false;
    }
    shared.ready.notify_all();
}

fn panic_message(payload: &(dyn std::any::Any + Send)) -> String {
    if let Some(message) = payload.downcast_ref::<&str>() {
        (*message).to_owned()
    } else if let Some(message) = payload.downcast_ref::<String>() {
        message.clone()
    } else {
        "scheduled work panicked with a non-string payload".to_owned()
    }
}

#[cfg(test)]
mod tests {
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
}
