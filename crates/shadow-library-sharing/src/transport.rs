use std::io::{self, Read, Write};

use thiserror::Error;

use crate::protocol::{RequestEnvelope, ResponseHeader};

const MAX_JSON_FRAME_BYTES: usize = 1_048_576;
const MAX_BINARY_BODY_BYTES: usize = 64 * 1_024 * 1_024;

#[derive(Debug, Error)]
pub enum TransportError {
    #[error("remote Library transport I/O failed: {0}")]
    Io(#[from] io::Error),
    #[error("remote Library JSON frame is {actual} bytes, above the {maximum} byte limit")]
    FrameTooLarge { actual: usize, maximum: usize },
    #[error("remote Library JSON is invalid: {0}")]
    Json(#[from] serde_json::Error),
    #[error("remote Library response body is {actual} bytes, expected {expected}")]
    BodyLength { actual: usize, expected: u64 },
}

pub(crate) fn write_request(
    stream: &mut impl Write,
    request: &RequestEnvelope,
) -> Result<(), TransportError> {
    write_json_frame(stream, request)
}

pub(crate) fn read_request(stream: &mut impl Read) -> Result<RequestEnvelope, TransportError> {
    read_json_frame(stream)
}

pub(crate) fn write_response(
    stream: &mut impl Write,
    header: &ResponseHeader,
    body: &[u8],
) -> Result<(), TransportError> {
    let actual = u64::try_from(body.len()).unwrap_or(u64::MAX);
    if actual != header.body_byte_len {
        return Err(TransportError::BodyLength {
            actual: body.len(),
            expected: header.body_byte_len,
        });
    }
    write_json_frame(stream, header)?;
    stream.write_all(body)?;
    stream.flush()?;
    Ok(())
}

pub(crate) fn read_response(
    stream: &mut impl Read,
) -> Result<(ResponseHeader, Vec<u8>), TransportError> {
    let header: ResponseHeader = read_json_frame(stream)?;
    let body_len =
        usize::try_from(header.body_byte_len).map_err(|_| TransportError::FrameTooLarge {
            actual: usize::MAX,
            maximum: usize::MAX,
        })?;
    if body_len > MAX_BINARY_BODY_BYTES {
        return Err(TransportError::FrameTooLarge {
            actual: body_len,
            maximum: MAX_BINARY_BODY_BYTES,
        });
    }
    let mut body = vec![0_u8; body_len];
    stream.read_exact(&mut body)?;
    Ok((header, body))
}

fn write_json_frame<T: serde::Serialize>(
    stream: &mut impl Write,
    value: &T,
) -> Result<(), TransportError> {
    let bytes = serde_json::to_vec(value)?;
    if bytes.len() > MAX_JSON_FRAME_BYTES {
        return Err(TransportError::FrameTooLarge {
            actual: bytes.len(),
            maximum: MAX_JSON_FRAME_BYTES,
        });
    }
    let length = u32::try_from(bytes.len()).map_err(|_| TransportError::FrameTooLarge {
        actual: bytes.len(),
        maximum: MAX_JSON_FRAME_BYTES,
    })?;
    stream.write_all(&length.to_be_bytes())?;
    stream.write_all(&bytes)?;
    stream.flush()?;
    Ok(())
}

fn read_json_frame<T: serde::de::DeserializeOwned>(
    stream: &mut impl Read,
) -> Result<T, TransportError> {
    let mut length_bytes = [0_u8; 4];
    stream.read_exact(&mut length_bytes)?;
    let length = usize::try_from(u32::from_be_bytes(length_bytes)).unwrap_or(usize::MAX);
    if length > MAX_JSON_FRAME_BYTES {
        return Err(TransportError::FrameTooLarge {
            actual: length,
            maximum: MAX_JSON_FRAME_BYTES,
        });
    }
    let mut bytes = vec![0_u8; length];
    stream.read_exact(&mut bytes)?;
    serde_json::from_slice(&bytes).map_err(Into::into)
}

#[cfg(test)]
mod tests;
