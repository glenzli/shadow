//! Bounded helper-process execution and descendant cleanup.
//!
//! This owner contains the complete process lifecycle: environment isolation,
//! pipe draining, timeout observation, process-group termination, and bounded
//! diagnostics. Protocol interpretation belongs to the calling operation.

use std::{
    io::{self, Read},
    path::Path,
    process::{Child, Command, ExitStatus, Stdio},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, bail};

const DISABLE_PRIVATE_DECODER_ENVIRONMENT: &str = "SHADOW_DISABLE_PRIVATE_DECODER";
const MAX_HELPER_STREAM_BYTES: usize = 64 * 1024;
pub(super) const HELPER_TIMEOUT: Duration = Duration::from_secs(30);
const HELPER_POLL_INTERVAL: Duration = Duration::from_millis(10);
// A helper may exit while an SDK-created descendant still owns stdout/stderr.
// Never let those inherited descriptors turn a bounded decoder operation into
// an unbounded join in the desktop process.
const HELPER_PIPE_DRAIN_TIMEOUT: Duration = Duration::from_secs(2);

#[cfg(unix)]
unsafe extern "C" {
    fn killpg(pgrp: i32, sig: i32) -> i32;
}

#[derive(Debug)]
pub(super) struct HelperProcessOutput {
    pub(super) status: ExitStatus,
    pub(super) stdout: Vec<u8>,
    pub(super) stderr: Vec<u8>,
}

#[derive(Debug)]
pub(super) enum HelperExecution {
    Completed(HelperProcessOutput),
    TimedOut(HelperProcessOutput),
}

pub(super) fn execute_decode_helper(
    helper_path: &Path,
    configure: impl FnOnce(&mut Command),
) -> Result<HelperExecution> {
    execute_decode_helper_with_timeout(helper_path, HELPER_TIMEOUT, configure)
}

fn execute_decode_helper_with_timeout(
    helper_path: &Path,
    timeout: Duration,
    configure: impl FnOnce(&mut Command),
) -> Result<HelperExecution> {
    let mut command = Command::new(helper_path);
    command
        // The desktop host must not load a private native SDK. The helper is
        // its intentional isolation boundary, so it alone is allowed to probe
        // an installed private provider after the public decoder declines.
        .env_remove(DISABLE_PRIVATE_DECODER_ENVIRONMENT)
        // A malformed third-party provider must not be able to fill an
        // unbounded pipe before the timeout can be observed. Reader threads
        // keep both pipes draining while retaining a capped diagnostic tail.
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    configure_helper_process_group(&mut command);
    configure(&mut command);
    let mut child = command
        .spawn()
        .with_context(|| format!("start isolated RAW decoder {}", helper_path.display()))?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| anyhow::anyhow!("isolated RAW decoder did not expose stdout"))?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| anyhow::anyhow!("isolated RAW decoder did not expose stderr"))?;
    let stdout_reader = spawn_helper_pipe_reader(stdout);
    let stderr_reader = spawn_helper_pipe_reader(stderr);
    let started_at = Instant::now();
    let mut timed_out = false;
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if started_at.elapsed() >= timeout => {
                timed_out = true;
                terminate_helper_process_tree(&mut child);
                break child
                    .wait()
                    .context("wait for timed-out isolated RAW decoder")?;
            }
            Ok(None) => thread::sleep(HELPER_POLL_INTERVAL),
            Err(error) => {
                terminate_helper_process_tree(&mut child);
                let _ = child.wait();
                return Err(error).context("poll isolated RAW decoder");
            }
        }
    };
    // Do not use JoinHandle::join here: a private SDK may have spawned a
    // descendant which inherited one pipe. A successful direct-child exit
    // must not make the desktop block forever waiting for that unrelated EOF.
    let drain_deadline = Instant::now() + HELPER_PIPE_DRAIN_TIMEOUT;
    let stdout = receive_helper_pipe_until(&stdout_reader, "stdout", drain_deadline)?;
    let stderr = receive_helper_pipe_until(&stderr_reader, "stderr", drain_deadline)?;
    if stdout.is_none() || stderr.is_none() {
        // On Unix this kills the dedicated helper process group, including
        // ordinary descendants that kept a pipe open. On other platforms the
        // direct child has already been reaped, but the parent still returns
        // on the same bounded deadline instead of joining a stuck reader.
        terminate_helper_process_tree(&mut child);
        return Ok(HelperExecution::TimedOut(HelperProcessOutput {
            status,
            stdout: stdout.unwrap_or_default(),
            stderr: stderr.unwrap_or_default(),
        }));
    }
    let output = HelperProcessOutput {
        status,
        stdout: stdout.expect("checked above"),
        stderr: stderr.expect("checked above"),
    };
    Ok(if timed_out {
        HelperExecution::TimedOut(output)
    } else {
        HelperExecution::Completed(output)
    })
}

fn configure_helper_process_group(command: &mut Command) {
    #[cfg(unix)]
    {
        // The helper gets its own process group so a timeout can terminate
        // regular decoder descendants that inherited stdout/stderr as well.
        use std::os::unix::process::CommandExt as _;
        command.process_group(0);
    }
    #[cfg(not(unix))]
    let _ = command;
}

fn terminate_helper_process_tree(child: &mut Child) {
    #[cfg(unix)]
    {
        // The group id is the helper's PID because configure_helper_process_group
        // requested process_group(0). Ignore ESRCH: the child can exit in the
        // narrow interval between try_wait and this cleanup.
        if let Ok(process_group) = i32::try_from(child.id()) {
            // SAFETY: `killpg` receives a positive PID-derived group id and
            // SIGKILL. The child was spawned into its own group above, so this
            // never targets the desktop process group.
            let _ = unsafe { killpg(process_group, 9) };
        }
    }
    let _ = child.kill();
}

fn read_capped_stream(mut stream: impl Read) -> io::Result<Vec<u8>> {
    let mut bytes = Vec::new();
    let mut buffer = [0_u8; 4_096];
    loop {
        let count = stream.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        let remaining = MAX_HELPER_STREAM_BYTES.saturating_sub(bytes.len());
        bytes.extend_from_slice(&buffer[..count.min(remaining)]);
    }
    Ok(bytes)
}

fn spawn_helper_pipe_reader(
    stream: impl Read + Send + 'static,
) -> mpsc::Receiver<io::Result<Vec<u8>>> {
    let (sender, receiver) = mpsc::sync_channel(1);
    thread::spawn(move || {
        // If the parent returns after the bounded drain deadline, dropping the
        // receiver is intentional: the reader owns no desktop state and must
        // never keep the UI waiting for a hostile inherited descriptor.
        let _ = sender.send(read_capped_stream(stream));
    });
    receiver
}

fn receive_helper_pipe_until(
    reader: &mpsc::Receiver<io::Result<Vec<u8>>>,
    name: &'static str,
    deadline: Instant,
) -> Result<Option<Vec<u8>>> {
    match reader.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
        Ok(result) => result
            .with_context(|| format!("read isolated RAW decoder {name}"))
            .map(Some),
        Err(mpsc::RecvTimeoutError::Timeout) => Ok(None),
        Err(mpsc::RecvTimeoutError::Disconnected) => {
            bail!("isolated RAW decoder {name} reader disconnected")
        }
    }
}

pub(super) fn helper_exit_looks_like_crash(status: ExitStatus) -> bool {
    // Unix reports signal termination as `None`. Some launchers translate
    // SIGABRT/SIGSEGV to conventional 134/139 exit codes; Windows exception
    // statuses are signed negative `i32`s. A non-zero ordinary `return 1`
    // stays a normal decoder rejection and is not a native-crash quarantine.
    status
        .code()
        .is_none_or(|code| matches!(code, 134 | 139) || code < 0)
}

pub(super) fn helper_stderr_suffix(stderr: &[u8]) -> String {
    let detail = String::from_utf8_lossy(stderr).trim().to_owned();
    if detail.is_empty() {
        String::new()
    } else {
        format!(": {detail}")
    }
}

#[cfg(all(test, unix))]
mod tests;
