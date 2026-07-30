//! Bounded, cancellation-aware child-process completion for local providers.

use std::{
    io::{self, Read},
    process::{Child, ExitStatus},
    thread::{self, JoinHandle},
    time::Duration,
};

use crate::CancellationToken;

const MAX_CAPTURED_STREAM_BYTES: usize = 64 * 1024;

pub(super) struct CapturedChild {
    pub(super) status: ExitStatus,
    pub(super) stdout: Vec<u8>,
    #[allow(dead_code)]
    pub(super) stderr: Vec<u8>,
}

pub(super) enum ProcessFailure {
    Cancelled,
    Wait,
    Reader,
}

pub(super) fn wait_with_bounded_output(
    mut child: Child,
    cancellation: &CancellationToken,
    poll_interval: Duration,
) -> Result<CapturedChild, ProcessFailure> {
    let Some(stdout) = child.stdout.take() else {
        let _ = child.kill();
        let _ = child.wait();
        return Err(ProcessFailure::Reader);
    };
    let Some(stderr) = child.stderr.take() else {
        let _ = child.kill();
        let _ = child.wait();
        return Err(ProcessFailure::Reader);
    };
    let stdout_reader = thread::spawn(move || drain_bounded(stdout));
    let stderr_reader = thread::spawn(move || drain_bounded(stderr));

    let status = loop {
        if cancellation.is_cancelled() {
            let _ = child.kill();
            let _ = child.wait();
            join_reader(stdout_reader)?;
            join_reader(stderr_reader)?;
            return Err(ProcessFailure::Cancelled);
        }
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) => thread::sleep(poll_interval),
            Err(_) => {
                let _ = child.kill();
                let _ = child.wait();
                join_reader(stdout_reader)?;
                join_reader(stderr_reader)?;
                return Err(ProcessFailure::Wait);
            }
        }
    };

    Ok(CapturedChild {
        status,
        stdout: join_reader(stdout_reader)?,
        stderr: join_reader(stderr_reader)?,
    })
}

fn join_reader(reader: JoinHandle<io::Result<Vec<u8>>>) -> Result<Vec<u8>, ProcessFailure> {
    reader
        .join()
        .map_err(|_| ProcessFailure::Reader)?
        .map_err(|_| ProcessFailure::Reader)
}

fn drain_bounded(mut reader: impl Read) -> io::Result<Vec<u8>> {
    let mut captured = Vec::new();
    let mut buffer = [0_u8; 8 * 1024];
    loop {
        let read = reader.read(&mut buffer)?;
        if read == 0 {
            return Ok(captured);
        }
        let remaining = MAX_CAPTURED_STREAM_BYTES.saturating_sub(captured.len());
        captured.extend_from_slice(&buffer[..read.min(remaining)]);
    }
}
