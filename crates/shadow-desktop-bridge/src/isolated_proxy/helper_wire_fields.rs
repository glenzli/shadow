//! Scalar grammar shared by the helper's metadata-bearing wire protocols.
//!
//! Operation envelopes, field ordering, and result construction remain with
//! their semantic owners. This module only decodes the bounded fixed-width
//! values those protocols deliberately share.

use anyhow::{Context, Result, bail};

const MAX_RECEIPT_TEXT_BYTES: usize = 16 * 1024;

pub(super) fn decode_printable_identity_field(encoded: &str, label: &str) -> Result<String> {
    let decoded = decode_protocol_hex_text_field(encoded, label, MAX_RECEIPT_TEXT_BYTES, false)?;
    // Pipeline identities can carry a human-readable fallback reason, which
    // legitimately contains spaces. Newlines, tabs, and other controls remain
    // forbidden even though the child encoded the field as hex.
    if !decoded
        .bytes()
        .all(|byte| byte == b' ' || byte.is_ascii_graphic())
    {
        bail!("isolated helper {label} is not a printable identity");
    }
    Ok(decoded)
}

pub(super) fn decode_protocol_hex_text_field(
    encoded: &str,
    label: &str,
    maximum_bytes: usize,
    allow_empty: bool,
) -> Result<String> {
    if encoded == "-" {
        if allow_empty {
            return Ok(String::new());
        }
        bail!("isolated helper {label} must not be empty");
    }
    if encoded.is_empty() || !encoded.len().is_multiple_of(2) || encoded.len() / 2 > maximum_bytes {
        bail!("isolated helper {label} is not a bounded hex value");
    }
    let mut bytes = Vec::with_capacity(encoded.len() / 2);
    for pair in encoded.as_bytes().chunks_exact(2) {
        let Some(high) = receipt_hex_nibble(pair[0]) else {
            bail!("isolated helper {label} contains invalid hex");
        };
        let Some(low) = receipt_hex_nibble(pair[1]) else {
            bail!("isolated helper {label} contains invalid hex");
        };
        bytes.push((high << 4) | low);
    }
    let decoded =
        String::from_utf8(bytes).with_context(|| format!("decode isolated helper {label}"))?;
    if (!allow_empty && decoded.is_empty()) || decoded.chars().any(char::is_control) {
        bail!("isolated helper {label} contains invalid text");
    }
    Ok(decoded)
}

pub(super) fn parse_metadata_u64(encoded: &str, label: &str) -> Result<u64> {
    if encoded.len() != 16 {
        bail!("isolated RAW metadata {label} is not a fixed-width integer");
    }
    let mut value = 0_u64;
    for byte in encoded.bytes() {
        let Some(nibble) = receipt_hex_nibble(byte) else {
            bail!("isolated RAW metadata {label} contains invalid hex");
        };
        value = (value << 4) | u64::from(nibble);
    }
    Ok(value)
}

pub(super) fn parse_metadata_u32(encoded: &str, label: &str) -> Result<u32> {
    u32::try_from(parse_metadata_u64(encoded, label)?)
        .with_context(|| format!("isolated RAW metadata {label} exceeds u32"))
}

pub(super) fn parse_metadata_i32(encoded: &str, label: &str) -> Result<i32> {
    Ok(parse_metadata_u32(encoded, label)?.cast_signed())
}

pub(super) fn parse_metadata_i64(encoded: &str, label: &str) -> Result<i64> {
    Ok(parse_metadata_u64(encoded, label)?.cast_signed())
}

pub(super) fn parse_metadata_f64(encoded: &str, label: &str) -> Result<f64> {
    let value = f64::from_bits(parse_metadata_u64(encoded, label)?);
    if !value.is_finite() {
        bail!("isolated RAW metadata {label} is not finite");
    }
    Ok(value)
}

fn receipt_hex_nibble(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}
