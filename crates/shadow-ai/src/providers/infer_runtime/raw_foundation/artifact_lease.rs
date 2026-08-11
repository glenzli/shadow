//! Owner-only Unix handle transport for one RAW foundation lease.
//!
//! HTTP authentication and Job lifecycle remain in the parent owner. This
//! module validates the local socket and open objects, then performs the
//! bounded single-frame `SCM_RIGHTS` registration handshake.

use std::{
    fs,
    io::{IoSlice, Read},
    net::Shutdown,
    os::{
        fd::AsRawFd,
        unix::{
            fs::{FileTypeExt, MetadataExt, PermissionsExt},
            net::UnixStream,
        },
    },
    time::Duration,
};

use nix::{
    fcntl::{FcntlArg, OFlag, fcntl},
    sys::socket::{ControlMessage, MsgFlags, sendmsg},
    unistd::Uid,
};
use serde::{Deserialize, Serialize};

use super::{
    InferRawFoundationLeaseGrant, InferRawFoundationRegisteredLease, InferRuntimeClientError,
    validate_capability_id,
};

const REQUEST_SCHEMA: &str = "infer.artifact-lease.register";
const RESPONSE_SCHEMA: &str = "infer.artifact-lease.registered";
const SCHEMA_VERSION: &str = "20260811.1";
const MAX_FRAME_BYTES: usize = 4_096;
const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(2);

#[derive(Serialize)]
struct RegisterRequest<'a> {
    schema: &'static str,
    schema_version: &'static str,
    operation: &'static str,
    ticket_id: &'a str,
}

#[derive(Deserialize)]
struct RegisterResponse {
    schema: String,
    schema_version: String,
    lease_id: String,
    expires_at_unix_ms: u64,
}

pub(super) fn register(
    grant: InferRawFoundationLeaseGrant,
    input: &fs::File,
    output: &fs::File,
) -> Result<InferRawFoundationRegisteredLease, InferRuntimeClientError> {
    validate_handles(input, output, grant.width, grant.height)?;
    validate_socket(&grant.socket_path)?;
    let mut stream = UnixStream::connect(&grant.socket_path)
        .map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    stream
        .set_read_timeout(Some(HANDSHAKE_TIMEOUT))
        .map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    stream
        .set_write_timeout(Some(HANDSHAKE_TIMEOUT))
        .map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    validate_peer(&stream)?;

    let request = RegisterRequest {
        schema: REQUEST_SCHEMA,
        schema_version: SCHEMA_VERSION,
        operation: "register",
        ticket_id: &grant.ticket_id,
    };
    let mut frame =
        serde_json::to_vec(&request).map_err(InferRuntimeClientError::SerializeRequest)?;
    frame.push(b'\n');
    if frame.len() > MAX_FRAME_BYTES {
        return Err(InferRuntimeClientError::InvalidRawArtifactLeaseProtocol);
    }
    let descriptors = [input.as_raw_fd(), output.as_raw_fd()];
    let slices = [IoSlice::new(&frame)];
    let controls = [ControlMessage::ScmRights(&descriptors)];
    let sent = sendmsg::<()>(
        stream.as_raw_fd(),
        &slices,
        &controls,
        MsgFlags::empty(),
        None,
    )
    .map_err(nix_io)?;
    if sent != frame.len() {
        return Err(InferRuntimeClientError::RawArtifactLeaseIo(
            std::io::Error::new(
                std::io::ErrorKind::WriteZero,
                "partial artifact lease frame",
            ),
        ));
    }
    stream
        .shutdown(Shutdown::Write)
        .map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    let response: RegisterResponse = read_response(&mut stream)?;
    validate_capability_id(&response.lease_id, "RAW lease id is invalid")?;
    if response.schema != RESPONSE_SCHEMA
        || response.schema_version != SCHEMA_VERSION
        || response.expires_at_unix_ms != grant.expires_at_unix_ms
    {
        return Err(InferRuntimeClientError::InvalidRawArtifactLeaseProtocol);
    }
    let output = output
        .try_clone()
        .map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    Ok(InferRawFoundationRegisteredLease {
        job: grant.job,
        lease_id: response.lease_id,
        expires_at_unix_ms: response.expires_at_unix_ms,
        source_revision: grant.source_revision,
        width: grant.width,
        height: grant.height,
        execution_timeout: grant.execution_timeout,
        output,
    })
}

fn validate_handles(
    input: &fs::File,
    output: &fs::File,
    width: u32,
    height: u32,
) -> Result<(), InferRuntimeClientError> {
    let expected_bytes = u64::from(width)
        .checked_mul(u64::from(height))
        .and_then(|pixels| pixels.checked_mul(2))
        .ok_or(InferRuntimeClientError::InvalidRawArtifactLeaseHandle(
            "input byte count overflows",
        ))?;
    let input_metadata = input
        .metadata()
        .map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    let output_metadata = output
        .metadata()
        .map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    let expected_uid = Uid::effective().as_raw();
    if !input_metadata.file_type().is_file()
        || input_metadata.uid() != expected_uid
        || input_metadata.permissions().mode() & 0o077 != 0
        || input_metadata.nlink() != 1
        || input_metadata.len() != expected_bytes
    {
        return Err(InferRuntimeClientError::InvalidRawArtifactLeaseHandle(
            "input must be owner-only, regular, singly linked, and match the staging size",
        ));
    }
    if !output_metadata.file_type().is_file()
        || output_metadata.uid() != expected_uid
        || output_metadata.permissions().mode() & 0o077 != 0
        || output_metadata.nlink() != 1
        || output_metadata.len() != 0
    {
        return Err(InferRuntimeClientError::InvalidRawArtifactLeaseHandle(
            "output must be owner-only, regular, singly linked, and empty",
        ));
    }
    if input_metadata.dev() == output_metadata.dev()
        && input_metadata.ino() == output_metadata.ino()
    {
        return Err(InferRuntimeClientError::InvalidRawArtifactLeaseHandle(
            "input and output must be different open objects",
        ));
    }
    let input_flags = descriptor_flags(input)?;
    if input_flags & OFlag::O_ACCMODE != OFlag::O_RDONLY {
        return Err(InferRuntimeClientError::InvalidRawArtifactLeaseHandle(
            "input handle must be read-only",
        ));
    }
    let output_flags = descriptor_flags(output)?;
    if output_flags & OFlag::O_ACCMODE != OFlag::O_WRONLY || output_flags.contains(OFlag::O_APPEND)
    {
        return Err(InferRuntimeClientError::InvalidRawArtifactLeaseHandle(
            "output handle must be non-append write-only",
        ));
    }
    Ok(())
}

fn descriptor_flags(file: &fs::File) -> Result<OFlag, InferRuntimeClientError> {
    fcntl(file.as_raw_fd(), FcntlArg::F_GETFL)
        .map(OFlag::from_bits_truncate)
        .map_err(nix_io)
}

fn validate_socket(path: &std::path::Path) -> Result<(), InferRuntimeClientError> {
    let expected_uid = Uid::effective().as_raw();
    let parent = path
        .parent()
        .ok_or(InferRuntimeClientError::UnsafeRawArtifactLeaseEndpoint)?;
    let parent_metadata =
        fs::symlink_metadata(parent).map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    let socket_metadata =
        fs::symlink_metadata(path).map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    if !parent_metadata.file_type().is_dir()
        || parent_metadata.uid() != expected_uid
        || parent_metadata.permissions().mode() & 0o777 != 0o700
        || !socket_metadata.file_type().is_socket()
        || socket_metadata.uid() != expected_uid
        || socket_metadata.permissions().mode() & 0o777 != 0o600
    {
        return Err(InferRuntimeClientError::UnsafeRawArtifactLeaseEndpoint);
    }
    Ok(())
}

#[cfg(target_os = "macos")]
fn validate_peer(stream: &UnixStream) -> Result<(), InferRuntimeClientError> {
    let (uid, _gid) = nix::unistd::getpeereid(stream).map_err(nix_io)?;
    if uid != Uid::effective() {
        return Err(InferRuntimeClientError::UnsafeRawArtifactLeaseEndpoint);
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn validate_peer(stream: &UnixStream) -> Result<(), InferRuntimeClientError> {
    use nix::sys::socket::{getsockopt, sockopt::PeerCredentials};

    let credentials = getsockopt(stream, PeerCredentials).map_err(nix_io)?;
    if credentials.uid() != Uid::effective().as_raw() {
        return Err(InferRuntimeClientError::UnsafeRawArtifactLeaseEndpoint);
    }
    Ok(())
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn validate_peer(_stream: &UnixStream) -> Result<(), InferRuntimeClientError> {
    Err(InferRuntimeClientError::RawArtifactLeaseUnsupported)
}

fn read_response(stream: &mut UnixStream) -> Result<RegisterResponse, InferRuntimeClientError> {
    let mut bytes = Vec::new();
    stream
        .take((MAX_FRAME_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(InferRuntimeClientError::RawArtifactLeaseIo)?;
    if bytes.len() > MAX_FRAME_BYTES
        || bytes.last() != Some(&b'\n')
        || bytes[..bytes.len().saturating_sub(1)].contains(&b'\n')
    {
        return Err(InferRuntimeClientError::InvalidRawArtifactLeaseProtocol);
    }
    serde_json::from_slice(&bytes[..bytes.len() - 1])
        .map_err(|_| InferRuntimeClientError::InvalidRawArtifactLeaseProtocol)
}

fn nix_io(error: nix::errno::Errno) -> InferRuntimeClientError {
    InferRuntimeClientError::RawArtifactLeaseIo(std::io::Error::from_raw_os_error(error as i32))
}
