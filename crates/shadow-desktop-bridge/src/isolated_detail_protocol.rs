//! Bounded wire contract for one child-rendered full-resolution detail tile.
//!
//! This module intentionally has no desktop-facade dependency yet. The helper
//! owns RAW/provider state and writes one tightly packed display-sRGB RGB8
//! rectangle to a caller-controlled cache-root file; the future facade owns
//! request scheduling, cancellation and cache publication. Keeping the parser
//! here makes that later wiring a small lease rather than a second ad-hoc IPC
//! protocol.

use std::{
    fs,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use shadow_domain::ImageDimensions;

pub(crate) const NEUTRAL_DETAIL_TILE_PROTOCOL: &str = "shadow-detail-tile-v1";
pub(crate) const NEUTRAL_DETAIL_TILE_OPERATION: &str = "neutral-detail-tile";
pub(crate) const MAX_DETAIL_TILE_SIDE: u32 = 1_024;
pub(crate) const MAX_DETAIL_TILE_BYTES: u64 =
    (MAX_DETAIL_TILE_SIDE as u64) * (MAX_DETAIL_TILE_SIDE as u64) * 3;

/// A rectangle in the oriented, full-resolution display canvas.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) struct IsolatedDetailTileRect {
    pub(crate) x: u32,
    pub(crate) y: u32,
    pub(crate) width: u32,
    pub(crate) height: u32,
}

/// Parent-established invariants for one child detail request. The source
/// inspection stage already knows the oriented canvas; requiring it here
/// prevents a stale or misrouted child receipt from being published under the
/// right nonce but for a different tile.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct NeutralDetailTileExpectation {
    pub(crate) nonce: String,
    pub(crate) rect: IsolatedDetailTileRect,
    pub(crate) full_dimensions: ImageDimensions,
}

/// Receipt emitted on helper stdout after the tile file has been atomically
/// published. It carries no source path, recipe data, pixel bytes or native
/// handle. `raw_pipeline_identity` is provenance for a future cache key, not
/// permission for the desktop process to reopen the source.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct IsolatedDetailTileReceipt {
    pub(crate) nonce: String,
    pub(crate) rect: IsolatedDetailTileRect,
    pub(crate) full_dimensions: ImageDimensions,
    pub(crate) row_stride_bytes: u32,
    pub(crate) byte_len: u64,
    pub(crate) raw_pipeline_path: u8,
    pub(crate) raw_pipeline_identity: String,
}

/// Parses the fixed, nonce-bound helper receipt for the neutral full-detail
/// proof command. The next recipe-aware protocol will use a new operation and
/// add canonical recipe/geometry contracts rather than overloading this one.
pub(crate) fn parse_neutral_detail_tile_receipt(
    stdout: &[u8],
    expected: &NeutralDetailTileExpectation,
) -> Result<IsolatedDetailTileReceipt> {
    validate_expectation(expected)?;
    let response = std::str::from_utf8(stdout).context("decode isolated detail tile output")?;
    let fields = response.split_whitespace().collect::<Vec<_>>();
    // protocol, operation, nonce, rect x/y/w/h, full w/h, stride, bytes,
    // pipeline path, pipeline identity.
    if fields.len() != 13
        || fields[0] != NEUTRAL_DETAIL_TILE_PROTOCOL
        || fields[1] != NEUTRAL_DETAIL_TILE_OPERATION
        || fields[2] != expected.nonce
    {
        bail!("isolated detail tile returned an invalid protocol response");
    }
    let rect = IsolatedDetailTileRect {
        x: parse_u32(fields[3], "tile x")?,
        y: parse_u32(fields[4], "tile y")?,
        width: parse_u32(fields[5], "tile width")?,
        height: parse_u32(fields[6], "tile height")?,
    };
    let full_dimensions = ImageDimensions {
        width: parse_u32(fields[7], "full width")?,
        height: parse_u32(fields[8], "full height")?,
    };
    let row_stride_bytes = parse_u32(fields[9], "row stride")?;
    let byte_len = parse_u64(fields[10], "tile byte length")?;
    let raw_pipeline_path = u8::try_from(parse_u64(fields[11], "RAW pipeline path")?)
        .context("isolated detail tile RAW pipeline path exceeds u8")?;
    if !(1..=2).contains(&raw_pipeline_path) {
        bail!("isolated detail tile did not produce a RAW development route");
    }
    let raw_pipeline_identity = decode_hex_text(fields[12], "RAW pipeline identity", 16 * 1024)?;
    let receipt = IsolatedDetailTileReceipt {
        nonce: expected.nonce.clone(),
        rect,
        full_dimensions,
        row_stride_bytes,
        byte_len,
        raw_pipeline_path,
        raw_pipeline_identity,
    };
    validate_receipt_against_expectation(&receipt, expected)?;
    Ok(receipt)
}

/// Reads a helper-produced tile only after proving that it is a regular file
/// below the caller's cache root and matches the bounded receipt layout. The
/// caller should derive `output_path` from a fresh nonce; this function rejects
/// path traversal/symlink escapes instead of trusting a helper string.
pub(crate) fn read_verified_detail_tile(
    cache_root: &Path,
    output_path: &Path,
    receipt: &IsolatedDetailTileReceipt,
    expected: &NeutralDetailTileExpectation,
) -> Result<Vec<u8>> {
    validate_receipt_against_expectation(receipt, expected)?;
    let canonical_root = fs::canonicalize(cache_root).with_context(|| {
        format!(
            "canonicalize detail tile cache root {}",
            cache_root.display()
        )
    })?;
    let expected_output = neutral_detail_tile_output_path(cache_root, &expected.nonce)?;
    let canonical_expected = fs::canonicalize(&expected_output).with_context(|| {
        format!(
            "canonicalize nonce-bound detail tile output {}",
            expected_output.display()
        )
    })?;
    let canonical_output = fs::canonicalize(output_path)
        .with_context(|| format!("canonicalize detail tile output {}", output_path.display()))?;
    if canonical_output != canonical_expected || !canonical_output.starts_with(&canonical_root) {
        bail!("isolated detail tile output is not the nonce-bound cache artifact");
    }
    let metadata = fs::metadata(&canonical_output)
        .with_context(|| format!("stat isolated detail tile {}", canonical_output.display()))?;
    if !metadata.is_file() || metadata.len() != receipt.byte_len {
        bail!("isolated detail tile file does not match its receipt length");
    }
    let bytes = fs::read(&canonical_output)
        .with_context(|| format!("read isolated detail tile {}", canonical_output.display()))?;
    if u64::try_from(bytes.len()).ok() != Some(receipt.byte_len) {
        bail!("isolated detail tile changed while it was being read");
    }
    Ok(bytes)
}

/// Derives the only output location accepted by the initial protocol. The
/// fresh nonce keeps a stale tile from being mistaken for a later request;
/// callers must still bind the receipt and future cache key to source/recipe
/// identity before publication.
pub(crate) fn neutral_detail_tile_output_path(cache_root: &Path, nonce: &str) -> Result<PathBuf> {
    validate_nonce(nonce)?;
    Ok(cache_root
        .join("decode-helper")
        .join("detail-tiles")
        .join(format!("{nonce}.rgb")))
}

fn validate_receipt(receipt: &IsolatedDetailTileReceipt) -> Result<()> {
    validate_nonce(&receipt.nonce)?;
    let rect = receipt.rect;
    if rect.width == 0
        || rect.height == 0
        || rect.width > MAX_DETAIL_TILE_SIDE
        || rect.height > MAX_DETAIL_TILE_SIDE
        || receipt.full_dimensions.width == 0
        || receipt.full_dimensions.height == 0
        || rect
            .x
            .checked_add(rect.width)
            .is_none_or(|right| right > receipt.full_dimensions.width)
        || rect
            .y
            .checked_add(rect.height)
            .is_none_or(|bottom| bottom > receipt.full_dimensions.height)
    {
        bail!("isolated detail tile rectangle is outside its full-resolution canvas");
    }
    let expected_stride = rect
        .width
        .checked_mul(3)
        .ok_or_else(|| anyhow::anyhow!("isolated detail tile stride overflows"))?;
    let expected_len = u64::from(expected_stride)
        .checked_mul(u64::from(rect.height))
        .ok_or_else(|| anyhow::anyhow!("isolated detail tile byte length overflows"))?;
    if receipt.row_stride_bytes != expected_stride
        || receipt.byte_len != expected_len
        || receipt.byte_len > MAX_DETAIL_TILE_BYTES
        || receipt.raw_pipeline_identity.is_empty()
    {
        bail!("isolated detail tile receipt has an invalid RGB8 layout");
    }
    Ok(())
}

fn validate_expectation(expected: &NeutralDetailTileExpectation) -> Result<()> {
    validate_nonce(&expected.nonce)?;
    validate_receipt(&IsolatedDetailTileReceipt {
        nonce: expected.nonce.clone(),
        rect: expected.rect,
        full_dimensions: expected.full_dimensions,
        row_stride_bytes: expected
            .rect
            .width
            .checked_mul(3)
            .ok_or_else(|| anyhow::anyhow!("isolated detail tile expectation stride overflows"))?,
        byte_len: u64::from(expected.rect.width)
            .checked_mul(3)
            .and_then(|stride| stride.checked_mul(u64::from(expected.rect.height)))
            .ok_or_else(|| anyhow::anyhow!("isolated detail tile expectation length overflows"))?,
        // An expectation is only about the canvas. The receipt itself must
        // still prove that the child used a genuine RAW route.
        raw_pipeline_path: 1,
        raw_pipeline_identity: "expected-route-placeholder".to_owned(),
    })
}

fn validate_receipt_against_expectation(
    receipt: &IsolatedDetailTileReceipt,
    expected: &NeutralDetailTileExpectation,
) -> Result<()> {
    validate_expectation(expected)?;
    validate_receipt(receipt)?;
    if receipt.nonce != expected.nonce
        || receipt.rect != expected.rect
        || receipt.full_dimensions != expected.full_dimensions
    {
        bail!("isolated detail tile receipt does not match its requested canvas");
    }
    Ok(())
}

fn validate_nonce(nonce: &str) -> Result<()> {
    let valid = match nonce.len() {
        // `Uuid::simple()` is already used by the existing helper protocols.
        // Accept it here as well, while still keeping the token fixed-length.
        32 => nonce
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)),
        36 => nonce.bytes().enumerate().all(|(index, byte)| {
            let is_hyphen = matches!(index, 8 | 13 | 18 | 23);
            if is_hyphen {
                byte == b'-'
            } else {
                byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)
            }
        }),
        _ => false,
    };
    if !valid {
        bail!("isolated detail tile nonce must be a UUID-style token");
    }
    Ok(())
}

fn parse_u64(encoded: &str, label: &str) -> Result<u64> {
    if encoded.len() != 16 || !encoded.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        bail!("isolated detail tile {label} is not fixed-width hexadecimal");
    }
    u64::from_str_radix(encoded, 16).with_context(|| format!("parse isolated detail tile {label}"))
}

fn parse_u32(encoded: &str, label: &str) -> Result<u32> {
    u32::try_from(parse_u64(encoded, label)?)
        .with_context(|| format!("isolated detail tile {label} exceeds u32"))
}

fn decode_hex_text(encoded: &str, label: &str, maximum_bytes: usize) -> Result<String> {
    if encoded == "-" {
        bail!("isolated detail tile {label} must be present");
    }
    if encoded.len() % 2 != 0 || encoded.len() / 2 > maximum_bytes {
        bail!("isolated detail tile {label} exceeds its text bound");
    }
    let mut bytes = Vec::with_capacity(encoded.len() / 2);
    for pair in encoded.as_bytes().chunks_exact(2) {
        let high = hex_value(pair[0]).ok_or_else(|| anyhow::anyhow!("invalid {label} hex"))?;
        let low = hex_value(pair[1]).ok_or_else(|| anyhow::anyhow!("invalid {label} hex"))?;
        bytes.push(high << 4 | low);
    }
    let text = String::from_utf8(bytes).with_context(|| format!("decode detail tile {label}"))?;
    if text.chars().any(char::is_control) {
        bail!("isolated detail tile {label} contains control characters");
    }
    Ok(text)
}

const fn hex_value(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const NONCE: &str = "019f99ae-1234-4abc-8def-0123456789ab";

    fn receipt_text(nonce: &str) -> String {
        format!(
            "{NEUTRAL_DETAIL_TILE_PROTOCOL} {NEUTRAL_DETAIL_TILE_OPERATION} {nonce} \
             0000000000000000 0000000000000000 0000000000000100 0000000000000080 \
             0000000000001000 0000000000000c00 0000000000000300 0000000000018000 \
             0000000000000001 706970656c696e652d6964656e74697479\n"
        )
    }

    fn expectation(nonce: &str) -> NeutralDetailTileExpectation {
        NeutralDetailTileExpectation {
            nonce: nonce.to_owned(),
            rect: IsolatedDetailTileRect {
                x: 0,
                y: 0,
                width: 256,
                height: 128,
            },
            full_dimensions: ImageDimensions {
                width: 4_096,
                height: 3_072,
            },
        }
    }

    #[test]
    fn parses_a_nonce_bound_neutral_detail_tile_receipt() {
        let expected = expectation(NONCE);
        let receipt = parse_neutral_detail_tile_receipt(receipt_text(NONCE).as_bytes(), &expected)
            .expect("parse neutral detail tile receipt");
        assert_eq!(receipt.rect.width, 256);
        assert_eq!(receipt.rect.height, 128);
        assert_eq!(receipt.full_dimensions.width, 4096);
        assert_eq!(receipt.row_stride_bytes, 768);
        assert_eq!(receipt.byte_len, 98_304);
        assert_eq!(receipt.raw_pipeline_identity, "pipeline-identity");
    }

    #[test]
    fn rejects_stale_or_impossible_neutral_detail_tile_receipts() {
        assert!(parse_neutral_detail_tile_receipt(
            receipt_text(NONCE).as_bytes(),
            &expectation("other")
        )
        .is_err());
        let invalid = receipt_text(NONCE).replace("0000000000000100", "0000000000000800");
        assert!(parse_neutral_detail_tile_receipt(invalid.as_bytes(), &expectation(NONCE)).is_err());
    }

    #[test]
    fn accepts_the_existing_compact_uuid_nonce_form() {
        let nonce = "019f99ae12344abc8def0123456789ab";
        let receipt = parse_neutral_detail_tile_receipt(
            receipt_text(nonce).as_bytes(),
            &expectation(nonce),
        )
        .expect("parse compact UUID nonce receipt");
        assert_eq!(receipt.nonce, nonce);
    }

    #[test]
    fn accepts_only_cache_root_bound_tile_files() {
        let root =
            std::env::temp_dir().join(format!("shadow-detail-protocol-{}", uuid::Uuid::now_v7()));
        let cache_root = root.join("cache");
        fs::create_dir_all(cache_root.join("decode-helper/detail-tiles"))
            .expect("create detail cache directory");
        let expected = expectation(NONCE);
        let receipt = parse_neutral_detail_tile_receipt(receipt_text(NONCE).as_bytes(), &expected)
            .expect("parse receipt");
        let output = neutral_detail_tile_output_path(&cache_root, NONCE).expect("derive output");
        fs::write(
            &output,
            vec![0_u8; usize::try_from(receipt.byte_len).expect("tile fits")],
        )
        .expect("write tile");
        assert_eq!(
            read_verified_detail_tile(&cache_root, &output, &receipt, &expected)
                .expect("read verified tile")
                .len(),
            usize::try_from(receipt.byte_len).expect("tile fits")
        );
        fs::remove_dir_all(root).expect("remove detail protocol fixture");
    }

    #[test]
    fn rejects_another_file_below_the_same_cache_root() {
        let root =
            std::env::temp_dir().join(format!("shadow-detail-protocol-{}", uuid::Uuid::now_v7()));
        let cache_root = root.join("cache");
        fs::create_dir_all(cache_root.join("decode-helper/detail-tiles"))
            .expect("create detail cache directory");
        let expected = expectation(NONCE);
        let receipt = parse_neutral_detail_tile_receipt(receipt_text(NONCE).as_bytes(), &expected)
            .expect("parse receipt");
        let output = neutral_detail_tile_output_path(&cache_root, NONCE).expect("derive output");
        fs::write(
            &output,
            vec![0_u8; usize::try_from(receipt.byte_len).expect("tile fits")],
        )
        .expect("write expected tile");
        let another = cache_root.join("decode-helper/detail-tiles/another.rgb");
        fs::write(
            &another,
            vec![0_u8; usize::try_from(receipt.byte_len).expect("tile fits")],
        )
        .expect("write unrelated tile");
        assert!(read_verified_detail_tile(&cache_root, &another, &receipt, &expected).is_err());
        fs::remove_dir_all(root).expect("remove detail protocol fixture");
    }
}
