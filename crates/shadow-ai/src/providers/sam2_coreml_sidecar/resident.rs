//! Resident SAM sidecar session and bounded JSON-lines transport.
//!
//! One session owns one child process, its compiled Core ML models, and at most
//! one image embedding. Request-scoped providers borrow this aggregate through
//! a cloneable synchronized handle; a transport failure discards the entire
//! child and retries the current request once from its complete content
//! identity.

use std::{
    fmt::{self, Write as _},
    io::{self, BufRead as _, BufReader, BufWriter, Write as _},
    path::{Path, PathBuf},
    process::{Child, ChildStdin, Command, Stdio},
    sync::{
        Arc, Mutex, TryLockError,
        mpsc::{self, Receiver, RecvTimeoutError},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::{AdmittedModelIdentity, CancellationToken, MaskPointPolarity, MaskPromptPoint};

use super::{SAM2_COREML_EXACT_REVISION, VerifiedSam2CoreMlInstallation, remove_failed_output};

const RESIDENT_PROTOCOL_VERSION: u32 = 1;
const POLL_INTERVAL: Duration = Duration::from_millis(10);
const STARTUP_TIMEOUT: Duration = Duration::from_secs(30);
const IMAGE_TIMEOUT: Duration = Duration::from_secs(30);
const PREDICTION_TIMEOUT: Duration = Duration::from_secs(15);
const MAXIMUM_LINE_BYTES: usize = 64 * 1024;
const MAXIMUM_DIAGNOSTIC_BYTES: usize = 64 * 1024;
const MAXIMUM_ATTEMPTS: usize = 2;

#[derive(Clone)]
pub struct Sam2CoreMlResidentSession {
    model_identity: AdmittedModelIdentity,
    state: Arc<Mutex<SessionState>>,
}

impl fmt::Debug for Sam2CoreMlResidentSession {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Sam2CoreMlResidentSession")
            .finish_non_exhaustive()
    }
}

impl Sam2CoreMlResidentSession {
    pub fn new(installation: &VerifiedSam2CoreMlInstallation) -> Self {
        Self {
            model_identity: installation.model_identity.clone(),
            state: Arc::new(Mutex::new(SessionState {
                executable: installation.executable.clone(),
                model_directory: installation.model_directory.clone(),
                manifest_path: installation.manifest_path.clone(),
                process: None,
            })),
        }
    }

    pub(super) fn matches_installation(
        &self,
        installation: &VerifiedSam2CoreMlInstallation,
    ) -> bool {
        self.model_identity == installation.model_identity
    }

    pub(super) fn predict(
        &self,
        input_jpeg: &Path,
        input_content_hash: &str,
        output_mask: &Path,
        points: &[MaskPromptPoint],
        cancellation: &CancellationToken,
    ) -> Result<ResidentPredictionReceipt, ResidentSessionFailure> {
        let mut state = loop {
            if cancellation.is_cancelled() {
                return Err(ResidentSessionFailure::Cancelled);
            }
            match self.state.try_lock() {
                Ok(state) => break state,
                Err(TryLockError::WouldBlock) => thread::sleep(POLL_INTERVAL),
                Err(TryLockError::Poisoned(_)) => {
                    return Err(ResidentSessionFailure::StatePoisoned);
                }
            }
        };
        state.predict(
            input_jpeg,
            input_content_hash,
            output_mask,
            points,
            cancellation,
        )
    }
}

struct SessionState {
    executable: PathBuf,
    model_directory: PathBuf,
    manifest_path: PathBuf,
    process: Option<ResidentProcess>,
}

impl SessionState {
    fn predict(
        &mut self,
        input_jpeg: &Path,
        input_content_hash: &str,
        output_mask: &Path,
        points: &[MaskPromptPoint],
        cancellation: &CancellationToken,
    ) -> Result<ResidentPredictionReceipt, ResidentSessionFailure> {
        if input_content_hash.is_empty() {
            return Err(ResidentSessionFailure::InvalidInputIdentity);
        }
        let input_jpeg = input_jpeg
            .to_str()
            .ok_or(ResidentSessionFailure::InvalidPath)?;
        let output_mask_text = output_mask
            .to_str()
            .ok_or(ResidentSessionFailure::InvalidPath)?;
        let wire_points = points
            .iter()
            .map(|point| WirePoint {
                x: point.x.get(),
                y: point.y.get(),
                foreground: point.polarity == MaskPointPolarity::Foreground,
            })
            .collect::<Vec<_>>();

        let mut final_failure = ResidentSessionFailure::Transport;
        for attempt in 0..MAXIMUM_ATTEMPTS {
            if cancellation.is_cancelled() {
                self.reset_process();
                remove_failed_output(output_mask);
                return Err(ResidentSessionFailure::Cancelled);
            }
            let result = self.predict_once(
                input_jpeg,
                input_content_hash,
                output_mask_text,
                &wire_points,
                cancellation,
            );
            match result {
                Ok(receipt) => return Ok(receipt),
                Err(ResidentSessionFailure::Cancelled) => {
                    self.reset_process();
                    remove_failed_output(output_mask);
                    return Err(ResidentSessionFailure::Cancelled);
                }
                Err(failure) if attempt + 1 < MAXIMUM_ATTEMPTS && failure.is_retryable() => {
                    final_failure = failure;
                    self.reset_process();
                    remove_failed_output(output_mask);
                }
                Err(failure) => {
                    self.reset_process();
                    remove_failed_output(output_mask);
                    return Err(failure);
                }
            }
        }
        Err(final_failure)
    }

    fn predict_once(
        &mut self,
        input_jpeg: &str,
        input_content_hash: &str,
        output_mask: &str,
        points: &[WirePoint],
        cancellation: &CancellationToken,
    ) -> Result<ResidentPredictionReceipt, ResidentSessionFailure> {
        if self.process.is_none() {
            self.process = Some(ResidentProcess::spawn(
                &self.executable,
                &self.model_directory,
                &self.manifest_path,
                cancellation,
            )?);
        }
        let process = self
            .process
            .as_mut()
            .expect("resident process was inserted");
        if process.loaded_input_hash.as_deref() != Some(input_content_hash) {
            let request_id = process.next_request_id()?;
            process.send(&json!({
                "protocol": RESIDENT_PROTOCOL_VERSION,
                "id": request_id,
                "op": "load_image",
                "input_jpeg": input_jpeg,
                "input_hash": input_content_hash,
            }))?;
            let response =
                process.wait_response(request_id, "load_image", cancellation, IMAGE_TIMEOUT)?;
            response.require_success()?;
            process.loaded_input_hash = Some(input_content_hash.to_owned());
        }

        let request_id = process.next_request_id()?;
        process.send(&json!({
            "protocol": RESIDENT_PROTOCOL_VERSION,
            "id": request_id,
            "op": "predict",
            "input_hash": input_content_hash,
            "output_mask": output_mask,
            "points": points,
        }))?;
        let response =
            process.wait_response(request_id, "predict", cancellation, PREDICTION_TIMEOUT)?;
        response.require_success()?;
        let receipt = ResidentPredictionReceipt {
            width: response
                .width
                .ok_or_else(|| ResidentSessionFailure::InvalidResponse("missing_width".into()))?,
            height: response
                .height
                .ok_or_else(|| ResidentSessionFailure::InvalidResponse("missing_height".into()))?,
            score: response
                .score
                .ok_or_else(|| ResidentSessionFailure::InvalidResponse("missing_score".into()))?,
            points: response
                .points
                .ok_or_else(|| ResidentSessionFailure::InvalidResponse("missing_points".into()))?,
        };
        if receipt.width == 0
            || receipt.height == 0
            || !receipt.score.is_finite()
            || !(0.0..=1.0).contains(&receipt.score)
        {
            return Err(ResidentSessionFailure::InvalidResponse(
                "invalid_prediction_receipt".into(),
            ));
        }
        Ok(receipt)
    }

    fn reset_process(&mut self) {
        self.process.take();
    }
}

struct ResidentProcess {
    child: Child,
    stdin: Option<BufWriter<ChildStdin>>,
    events: Receiver<ReaderEvent>,
    stdout_reader: Option<JoinHandle<()>>,
    stderr_reader: Option<JoinHandle<io::Result<Vec<u8>>>>,
    next_request_id: u64,
    loaded_input_hash: Option<String>,
}

impl ResidentProcess {
    fn spawn(
        executable: &Path,
        model_directory: &Path,
        manifest_path: &Path,
        cancellation: &CancellationToken,
    ) -> Result<Self, ResidentSessionFailure> {
        let mut command = Command::new(executable);
        command
            .arg("--model-dir")
            .arg(model_directory)
            .arg("--manifest")
            .arg(manifest_path)
            .arg("--serve")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = command.spawn().map_err(|_| ResidentSessionFailure::Spawn)?;
        let Some(stdin) = child.stdin.take() else {
            terminate_child(&mut child);
            return Err(ResidentSessionFailure::Transport);
        };
        let Some(stdout) = child.stdout.take() else {
            terminate_child(&mut child);
            return Err(ResidentSessionFailure::Transport);
        };
        let Some(stderr) = child.stderr.take() else {
            terminate_child(&mut child);
            return Err(ResidentSessionFailure::Transport);
        };
        let (sender, events) = mpsc::channel();
        let stdout_reader = thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                let mut line = Vec::new();
                match reader.read_until(b'\n', &mut line) {
                    Ok(0) => {
                        let _ = sender.send(ReaderEvent::Eof);
                        return;
                    }
                    Ok(_) if line.len() <= MAXIMUM_LINE_BYTES => {
                        while matches!(line.last(), Some(b'\n' | b'\r')) {
                            line.pop();
                        }
                        if sender.send(ReaderEvent::Line(line)).is_err() {
                            return;
                        }
                    }
                    Ok(_) | Err(_) => {
                        let _ = sender.send(ReaderEvent::Invalid);
                        return;
                    }
                }
            }
        });
        let stderr_reader = thread::spawn(move || drain_bounded(stderr));
        let mut process = Self {
            child,
            stdin: Some(BufWriter::new(stdin)),
            events,
            stdout_reader: Some(stdout_reader),
            stderr_reader: Some(stderr_reader),
            next_request_id: 0,
            loaded_input_hash: None,
        };
        let ready = process.wait_message(cancellation, STARTUP_TIMEOUT)?;
        if ready.protocol != RESIDENT_PROTOCOL_VERSION
            || ready.kind != "ready"
            || ready.revision.as_deref() != Some(SAM2_COREML_EXACT_REVISION)
        {
            return Err(ResidentSessionFailure::InvalidResponse(
                "ready_contract".into(),
            ));
        }
        Ok(process)
    }

    fn next_request_id(&mut self) -> Result<u64, ResidentSessionFailure> {
        self.next_request_id = self
            .next_request_id
            .checked_add(1)
            .ok_or(ResidentSessionFailure::RequestIdentityExhausted)?;
        Ok(self.next_request_id)
    }

    fn send(&mut self, request: &serde_json::Value) -> Result<(), ResidentSessionFailure> {
        let encoded = serde_json::to_vec(request).map_err(|_| ResidentSessionFailure::Transport)?;
        if encoded.len() > MAXIMUM_LINE_BYTES {
            return Err(ResidentSessionFailure::Transport);
        }
        let stdin = self
            .stdin
            .as_mut()
            .ok_or(ResidentSessionFailure::Transport)?;
        stdin
            .write_all(&encoded)
            .and_then(|()| stdin.write_all(b"\n"))
            .and_then(|()| stdin.flush())
            .map_err(|_| ResidentSessionFailure::Transport)
    }

    fn wait_response(
        &mut self,
        expected_id: u64,
        expected_operation: &str,
        cancellation: &CancellationToken,
        timeout: Duration,
    ) -> Result<WireResponse, ResidentSessionFailure> {
        let response = self.wait_message(cancellation, timeout)?;
        if response.protocol != RESIDENT_PROTOCOL_VERSION
            || response.kind != "response"
            || response.id != Some(expected_id)
            || response.operation.as_deref() != Some(expected_operation)
        {
            return Err(ResidentSessionFailure::InvalidResponse(
                "response_identity".into(),
            ));
        }
        Ok(response)
    }

    fn wait_message(
        &mut self,
        cancellation: &CancellationToken,
        timeout: Duration,
    ) -> Result<WireResponse, ResidentSessionFailure> {
        let deadline = Instant::now() + timeout;
        loop {
            if cancellation.is_cancelled() {
                self.terminate();
                return Err(ResidentSessionFailure::Cancelled);
            }
            if Instant::now() >= deadline {
                self.terminate();
                return Err(ResidentSessionFailure::Timeout);
            }
            match self.events.recv_timeout(POLL_INTERVAL) {
                Ok(ReaderEvent::Line(line)) => {
                    return serde_json::from_slice(&line).map_err(|error| {
                        ResidentSessionFailure::InvalidResponse(format!(
                            "json_decode: {error}; line={}",
                            bounded_wire_line(&line)
                        ))
                    });
                }
                Ok(ReaderEvent::Eof | ReaderEvent::Invalid)
                | Err(RecvTimeoutError::Disconnected) => {
                    return Err(ResidentSessionFailure::Transport);
                }
                Err(RecvTimeoutError::Timeout) => match self.child.try_wait() {
                    Ok(Some(_)) | Err(_) => return Err(ResidentSessionFailure::Transport),
                    Ok(None) => {}
                },
            }
        }
    }

    fn terminate(&mut self) {
        self.stdin.take();
        terminate_child(&mut self.child);
        if let Some(reader) = self.stdout_reader.take() {
            let _ = reader.join();
        }
        if let Some(reader) = self.stderr_reader.take() {
            let _ = reader.join();
        }
        self.loaded_input_hash = None;
    }
}

impl Drop for ResidentProcess {
    fn drop(&mut self) {
        self.terminate();
    }
}

enum ReaderEvent {
    Line(Vec<u8>),
    Eof,
    Invalid,
}

#[derive(Debug, Deserialize)]
struct WireResponse {
    protocol: u32,
    #[serde(rename = "type")]
    kind: String,
    id: Option<u64>,
    #[serde(rename = "op")]
    operation: Option<String>,
    ok: Option<bool>,
    revision: Option<String>,
    error: Option<String>,
    width: Option<u32>,
    height: Option<u32>,
    score: Option<f64>,
    points: Option<usize>,
}

impl WireResponse {
    fn require_success(&self) -> Result<(), ResidentSessionFailure> {
        if self.ok == Some(true) {
            Ok(())
        } else {
            let _has_bounded_error = self
                .error
                .as_deref()
                .is_some_and(|error| !error.is_empty() && error.len() <= 128);
            Err(ResidentSessionFailure::ProviderRejected)
        }
    }
}

#[derive(Debug, Serialize)]
struct WirePoint {
    x: f64,
    y: f64,
    foreground: bool,
}

#[derive(Debug)]
pub(super) struct ResidentPredictionReceipt {
    pub(super) width: u32,
    pub(super) height: u32,
    pub(super) score: f64,
    pub(super) points: usize,
}

#[derive(Debug)]
pub(super) enum ResidentSessionFailure {
    Cancelled,
    StatePoisoned,
    InvalidInputIdentity,
    InvalidPath,
    Spawn,
    Transport,
    Timeout,
    InvalidResponse(String),
    ProviderRejected,
    RequestIdentityExhausted,
}

impl ResidentSessionFailure {
    const fn is_retryable(&self) -> bool {
        matches!(
            self,
            Self::Spawn | Self::Transport | Self::Timeout | Self::InvalidResponse(_)
        )
    }
}

impl fmt::Display for ResidentSessionFailure {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Cancelled => formatter.write_str("cancelled"),
            Self::StatePoisoned => formatter.write_str("state_poisoned"),
            Self::InvalidInputIdentity => formatter.write_str("invalid_input_identity"),
            Self::InvalidPath => formatter.write_str("invalid_path"),
            Self::Spawn => formatter.write_str("spawn"),
            Self::Transport => formatter.write_str("transport"),
            Self::Timeout => formatter.write_str("timeout"),
            Self::InvalidResponse(detail) => write!(formatter, "invalid_response:{detail}"),
            Self::ProviderRejected => formatter.write_str("provider_rejected"),
            Self::RequestIdentityExhausted => formatter.write_str("request_identity_exhausted"),
        }
    }
}

fn terminate_child(child: &mut Child) {
    let _ = child.kill();
    let _ = child.wait();
}

fn drain_bounded(mut reader: impl io::Read) -> io::Result<Vec<u8>> {
    let mut captured = Vec::new();
    let mut buffer = [0_u8; 8 * 1024];
    loop {
        let read = reader.read(&mut buffer)?;
        if read == 0 {
            return Ok(captured);
        }
        let remaining = MAXIMUM_DIAGNOSTIC_BYTES.saturating_sub(captured.len());
        captured.extend_from_slice(&buffer[..read.min(remaining)]);
    }
}

fn bounded_wire_line(line: &[u8]) -> String {
    let mut rendered = String::new();
    for character in String::from_utf8_lossy(line).chars().take(256) {
        for escaped in character.escape_default() {
            let _ = rendered.write_char(escaped);
        }
    }
    rendered
}

#[cfg(test)]
mod tests;
