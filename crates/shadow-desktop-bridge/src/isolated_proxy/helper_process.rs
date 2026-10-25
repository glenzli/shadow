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
    configure_helper_process(&mut command);
    configure(&mut command);
    let mut child = command
        .spawn()
        .with_context(|| format!("start isolated RAW decoder {}", helper_path.display()))?;
    let process_tree = match HelperProcessTree::attach(&mut child) {
        Ok(tree) => tree,
        Err(error) => {
            // A failed Windows Job assignment must not turn process isolation
            // into an escaped decoder process.
            let _ = child.kill();
            let _ = child.wait();
            return Err(error);
        }
    };
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
                terminate_helper_process_tree(&mut child, &process_tree);
                break child
                    .wait()
                    .context("wait for timed-out isolated RAW decoder")?;
            }
            Ok(None) => thread::sleep(HELPER_POLL_INTERVAL),
            Err(error) => {
                terminate_helper_process_tree(&mut child, &process_tree);
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
        // This kills the Unix helper process group or the Windows Job Object,
        // including ordinary descendants that kept a pipe open. The parent
        // then returns on the same bounded deadline instead of joining a
        // stuck reader.
        terminate_helper_process_tree(&mut child, &process_tree);
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

fn configure_helper_process(command: &mut Command) {
    #[cfg(unix)]
    {
        // The helper gets its own process group so a timeout can terminate
        // regular decoder descendants that inherited stdout/stderr as well.
        use std::os::unix::process::CommandExt as _;
        command.process_group(0);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt as _;
        // The worker still owns its redirected pipes and Job Object, but a GUI
        // launch must never allocate a visible console for a decoder operation.
        command.creation_flags(windows_sys::Win32::System::Threading::CREATE_NO_WINDOW);
    }
    #[cfg(not(any(unix, windows)))]
    let _ = command;
}

fn terminate_helper_process_tree(child: &mut Child, process_tree: &HelperProcessTree) {
    process_tree.terminate();
    #[cfg(unix)]
    {
        // The group id is the helper's PID because configure_helper_process
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

/// Owns the Windows process tree for exactly one isolated decoder operation.
/// Closing a configured Job Object is a final fail-safe on every return path;
/// explicit termination below also makes timeouts release inherited pipes
/// promptly. Unix process-group setup remains the corresponding mechanism
/// there.
#[derive(Debug)]
struct HelperProcessTree {
    #[cfg(windows)]
    job: windows_sys::Win32::Foundation::HANDLE,
}

impl HelperProcessTree {
    fn attach(child: &mut Child) -> Result<Self> {
        #[cfg(windows)]
        {
            use std::{mem, os::windows::io::AsRawHandle as _, ptr};
            use windows_sys::Win32::{
                Foundation::{CloseHandle, HANDLE},
                System::JobObjects::{
                    AssignProcessToJobObject, CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JobObjectExtendedLimitInformation,
                    SetInformationJobObject,
                },
            };

            // SAFETY: null security attributes and name request the default,
            // unnamed Job Object owned by this process.
            let job = unsafe { CreateJobObjectW(ptr::null(), ptr::null()) };
            if job.is_null() {
                return Err(std::io::Error::last_os_error()).context("create decoder Job Object");
            }
            let mut limits: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = unsafe { mem::zeroed() };
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            // SAFETY: `limits` is initialized for the documented information
            // class and lives until the call returns.
            let limits_set = unsafe {
                SetInformationJobObject(
                    job,
                    JobObjectExtendedLimitInformation,
                    (&raw const limits).cast(),
                    mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                )
            };
            if limits_set == 0 {
                // SAFETY: this branch owns the just-created valid handle.
                unsafe { CloseHandle(job) };
                return Err(std::io::Error::last_os_error())
                    .context("configure decoder Job Object cleanup");
            }
            let process: HANDLE = child.as_raw_handle().cast();
            // SAFETY: `process` is the live direct child handle and `job` is
            // the configured Job Object above.
            if unsafe { AssignProcessToJobObject(job, process) } == 0 {
                // SAFETY: closing the Job Object kills no assigned process on
                // this error path and releases the handle we own.
                unsafe { CloseHandle(job) };
                return Err(std::io::Error::last_os_error())
                    .context("assign isolated decoder to Job Object");
            }
            return Ok(Self { job });
        }
        #[cfg(not(windows))]
        {
            let _ = child;
            Ok(Self {})
        }
    }

    fn terminate(&self) {
        #[cfg(windows)]
        {
            use windows_sys::Win32::System::JobObjects::TerminateJobObject;
            // SAFETY: this object exclusively owns the Job Object handle.
            // Ignore a race with an already-exited helper tree.
            let _ = unsafe { TerminateJobObject(self.job, 1) };
        }
    }
}

#[cfg(windows)]
impl Drop for HelperProcessTree {
    fn drop(&mut self) {
        use windows_sys::Win32::Foundation::CloseHandle;
        // SAFETY: the Job Object handle was created by attach and is released
        // exactly once here. KILL_ON_JOB_CLOSE is the shutdown backstop.
        unsafe { CloseHandle(self.job) };
    }
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

#[cfg(test)]
mod tests;
