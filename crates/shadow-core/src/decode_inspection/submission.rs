//! Non-blocking submission handle, bounded admission, and completion tickets.

use std::{
    path::Path,
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
        mpsc::{self, Receiver, Sender, SyncSender},
    },
    thread,
    time::{Duration, Instant},
};

use shadow_domain::RepresentationKind;

use crate::import::ScanCancellation;
use crate::performance::DecodePerformance;

use super::contract::{
    DecodeInspectionDiscardReason, DecodeInspectionError, DecodeInspectionOutcome,
    DecodeInspectionProgress, DecodeInspectionRequest,
};
use super::runtime_state::DecodeInspectionState;

#[derive(Debug, Clone)]
pub struct DecodeInspectionHandle {
    pub(super) senders: Arc<[SyncSender<Message>]>,
    pub(super) next_worker: Arc<AtomicUsize>,
    pub(super) state: Arc<Mutex<DecodeInspectionState>>,
    pub(super) provider_id: Arc<str>,
    pub(super) provider_version: Arc<str>,
    pub(super) proxy_variant_key: Arc<str>,
    pub(super) technical_preprocessing_version: Option<Arc<str>>,
    pub(super) supports_original_raw: bool,
    pub(super) supported_original_raster_extensions: Arc<[String]>,
    pub(super) caches_previews: bool,
    pub(super) profiled: bool,
}

#[derive(Debug)]
pub struct DecodeInspectionTicket {
    receiver: Receiver<Result<DecodeInspectionOutcome, DecodeInspectionError>>,
}

pub(super) enum Message {
    Inspect(InspectionMessage),
    Shutdown(SyncSender<DecodePerformance>),
}

pub(super) struct InspectionMessage {
    pub(super) request: DecodeInspectionRequest,
    pub(super) cancellation: ScanCancellation,
    pub(super) response: Sender<Result<DecodeInspectionOutcome, DecodeInspectionError>>,
    pub(super) enqueued_at: Option<Instant>,
}

pub(crate) struct DecodeInspectionSubmission {
    pub ticket: DecodeInspectionTicket,
    pub queue_full_events: u64,
}

impl DecodeInspectionHandle {
    pub fn provider_id(&self) -> &str {
        &self.provider_id
    }

    pub fn provider_version(&self) -> &str {
        &self.provider_version
    }

    pub fn proxy_variant_key(&self) -> &str {
        &self.proxy_variant_key
    }

    /// Returns whether this worker explicitly supports a concrete directly
    /// imported source path.
    ///
    /// This is intentionally a handle property, captured when the actor is
    /// created, so a folder scan can reject unsupported files before it puts
    /// work into the bounded inspection queue. Original raster support is
    /// extension-specific: it is not enough to know that a source is merely
    /// `OriginalRaster`.
    pub fn supports_source(&self, kind: RepresentationKind, path: &Path) -> bool {
        match kind {
            RepresentationKind::OriginalRaw => self.supports_original_raw,
            RepresentationKind::OriginalRaster => path
                .extension()
                .and_then(|extension| extension.to_str())
                .is_some_and(|extension| {
                    self.supported_original_raster_extensions
                        .iter()
                        .any(|supported| supported.eq_ignore_ascii_case(extension))
                }),
            RepresentationKind::DerivedDng
            | RepresentationKind::EmbeddedPreview
            | RepresentationKind::SceneLinearRgb
            | RepresentationKind::VendorRenderedRgb
            | RepresentationKind::Proxy => false,
        }
    }

    pub const fn caches_previews(&self) -> bool {
        self.caches_previews
    }

    pub fn technical_preprocessing_version(&self) -> Option<&str> {
        self.technical_preprocessing_version.as_deref()
    }

    /// Returns a cheap, non-blocking snapshot suitable for import progress.
    ///
    /// This intentionally reports visual publications separately from completed
    /// inspections: a Library can show an embedded camera preview while the
    /// same worker continues creating its deterministic generated proxy.
    #[must_use]
    pub fn progress_snapshot(&self) -> DecodeInspectionProgress {
        let state = self
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        DecodeInspectionProgress {
            summary: state.summary,
            visual_artifacts_published: state.visual_artifacts_published,
        }
    }

    /// Queues an inspection and returns immediately with a completion ticket.
    ///
    /// The bounded queue applies backpressure when imports outrun decoding.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError::WorkerUnavailable`] when the worker has
    /// already stopped.
    pub fn submit(
        &self,
        request: DecodeInspectionRequest,
    ) -> Result<DecodeInspectionTicket, DecodeInspectionError> {
        self.submit_with_cancellation(request, &ScanCancellation::new())
    }

    /// Queues an inspection governed by the same token as its parent scan.
    ///
    /// Waiting for space in the bounded queue remains cancellation-responsive.
    /// A job cancelled before provider work returns a discarded outcome rather
    /// than an error.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError::WorkerUnavailable`] after shutdown or
    /// worker disconnection.
    pub fn submit_with_cancellation(
        &self,
        request: DecodeInspectionRequest,
        cancellation: &ScanCancellation,
    ) -> Result<DecodeInspectionTicket, DecodeInspectionError> {
        self.submit_with_cancellation_inner(request, cancellation, false, || {})
            .map(|submission| submission.ticket)
    }

    pub(crate) fn submit_with_cancellation_observed(
        &self,
        request: DecodeInspectionRequest,
        cancellation: &ScanCancellation,
    ) -> Result<DecodeInspectionSubmission, DecodeInspectionError> {
        self.submit_with_cancellation_inner(request, cancellation, true, || {})
    }

    pub(super) fn submit_with_cancellation_inner(
        &self,
        request: DecodeInspectionRequest,
        cancellation: &ScanCancellation,
        observe_queue_full: bool,
        mut queue_full_hook: impl FnMut(),
    ) -> Result<DecodeInspectionSubmission, DecodeInspectionError> {
        let (response_sender, response_receiver) = mpsc::channel();
        let mut message = Some(Message::Inspect(InspectionMessage {
            request,
            cancellation: cancellation.clone(),
            response: response_sender,
            enqueued_at: self.profiled.then(Instant::now),
        }));
        let mut queue_full_events = 0_u64;
        let worker_count = self.senders.len();
        let mut first_worker = self.next_worker.fetch_add(1, Ordering::Relaxed) % worker_count;
        loop {
            let mut state = self
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if state.stopping {
                return Err(DecodeInspectionError::WorkerUnavailable);
            }
            if cancellation.is_cancelled() {
                if let Some(Message::Inspect(InspectionMessage { response, .. })) = message.take() {
                    let result = Ok(DecodeInspectionOutcome::Discarded(
                        DecodeInspectionDiscardReason::Cancelled,
                    ));
                    state.summary.record(&result);
                    let _ = response.send(result);
                }
                break;
            }
            let mut sent = false;
            let mut disconnected = false;
            for offset in 0..worker_count {
                let worker_index = (first_worker + offset) % worker_count;
                let pending = message
                    .take()
                    .expect("inspection message remains owned until submitted");
                match self.senders[worker_index].try_send(pending) {
                    Ok(()) => {
                        self.next_worker
                            .store(worker_index.wrapping_add(1), Ordering::Relaxed);
                        sent = true;
                        break;
                    }
                    Err(mpsc::TrySendError::Full(returned)) => {
                        message = Some(returned);
                    }
                    Err(mpsc::TrySendError::Disconnected(returned)) => {
                        message = Some(returned);
                        state.stopping = true;
                        disconnected = true;
                        break;
                    }
                }
            }
            drop(state);
            if sent {
                break;
            }
            if disconnected {
                return Err(DecodeInspectionError::WorkerUnavailable);
            }
            if observe_queue_full {
                queue_full_events = queue_full_events.saturating_add(1);
            }
            queue_full_hook();
            thread::park_timeout(Duration::from_millis(1));
            if let Some(Message::Inspect(inspection)) = &mut message
                && inspection.enqueued_at.is_some()
            {
                inspection.enqueued_at = Some(Instant::now());
            }
            first_worker = first_worker.wrapping_add(1) % worker_count;
        }
        Ok(DecodeInspectionSubmission {
            ticket: DecodeInspectionTicket {
                receiver: response_receiver,
            },
            queue_full_events,
        })
    }
}

impl DecodeInspectionTicket {
    /// Waits for this inspection without stopping the worker or later jobs.
    ///
    /// # Errors
    ///
    /// Returns the provider, filesystem, or catalog error produced by the job,
    /// or [`DecodeInspectionError::WorkerUnavailable`] after a disconnect.
    pub fn wait(self) -> Result<DecodeInspectionOutcome, DecodeInspectionError> {
        self.receiver
            .recv()
            .map_err(|_| DecodeInspectionError::WorkerUnavailable)?
    }

    /// Waits at most `timeout` for this inspection.
    ///
    /// # Errors
    ///
    /// Returns [`DecodeInspectionError::CompletionTimeout`] when the deadline
    /// expires, or the same job and disconnect errors as [`Self::wait`].
    pub fn wait_timeout(
        self,
        timeout: std::time::Duration,
    ) -> Result<DecodeInspectionOutcome, DecodeInspectionError> {
        self.receiver
            .recv_timeout(timeout)
            .map_err(|error| match error {
                mpsc::RecvTimeoutError::Timeout => {
                    DecodeInspectionError::CompletionTimeout(timeout)
                }
                mpsc::RecvTimeoutError::Disconnected => DecodeInspectionError::WorkerUnavailable,
            })?
    }
}
