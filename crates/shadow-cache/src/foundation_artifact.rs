//! Verified, bounded access to materialized AI RAW foundation artifacts.
//!
//! `.shadowrawf` stores demosaiced linear camera RGB as ordered planar
//! float32 stripes. Verification is intentionally distrustful: the complete
//! schema, cache identity, publication receipt, stripe coverage, every digest,
//! finite pixels, and the complete file are checked before any row is exposed.

use std::{
    fs::File,
    io::{self, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};
use thiserror::Error;

const FILE_MAGIC: &[u8; 8] = b"SHRAWF02";
const FOOTER_MAGIC: &[u8; 8] = b"SHRFEND1";
const HEADER_PREFIX_BYTES: usize = 16;
const HEADER_PREFIX_FILE_BYTES: u64 = 16;
const FOOTER_BYTES: usize = 56;
const FOOTER_FILE_BYTES: u64 = 56;
const MAX_JSON_BYTES: u64 = 16 * 1024 * 1024;
const HASH_CHUNK_BYTES: usize = 1024 * 1024;
const HEADER_SCHEMA: &str = "shadow-raw-foundation-header-v2";
const ARTIFACT_SCHEMA: &str = "shadow-raw-foundation-artifact-v2";
const CACHE_KEY_SCHEMA: &str = "shadow-raw-foundation-cache-key-v2";
const SEQUENCE_FORMAT: &str = "shadow-linear-camera-rgb-f32-stripe-chw-v1";
const IMPLEMENTATION_REVISION: &str = "rawnind-public-bayer-foundation-20260731.1";
const PACKAGE_SHA256: &str = "d71b5f1e727c85a359e6f74dca9e2016c9d8fc3e2f7ac3e9b347d80ceca969af";
const BAYER_GRAPH_SHA256: &str = "da27509dab6a2915da67e988acd86cf71f9d5bbc8d1aa0ed32933578a887b901";
const SOURCE_PIXEL_CONTRACT_SHA256: &str =
    "e1998069001c14d01251cc3d6e2bc2aa66b807f3f17d246e7ee7270528302f7f";
const UPSTREAM_RELEASE: &str = "release-5.6.0";
const UPSTREAM_REPOSITORY: &str = "https://github.com/darktable-org/darktable-ai";
const UPSTREAM_REVISION: &str = "5454d7aa6d89a67054fd4a83343b09e69acaf76a";
const TRAINING_REPOSITORY: &str = "https://github.com/trougnouf/rawnind_jddc";
const TRAINING_REVISION: &str = "4d455aa8ada69214eafa6a91ac0b2e011cf9dcb7";
const PUBLICATION_PRODUCER: &str = "rawnind-bayer-two-pass-stripe-v1";

#[derive(Debug, Error)]
pub enum FoundationArtifactError {
    #[error("foundation artifact I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("foundation artifact {part} is invalid JSON: {source}")]
    Json {
        part: &'static str,
        #[source]
        source: serde_json::Error,
    },
    #[error("invalid foundation artifact: {0}")]
    Invalid(&'static str),
    #[error(
        "foundation artifact identity mismatch: manifest records {recorded}, verifier computed {computed}"
    )]
    IdentityMismatch { recorded: String, computed: String },
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct FoundationArtifactStripe {
    pub index: usize,
    pub y_start: u32,
    pub rows: u32,
    pub offset: u64,
    pub byte_length: u64,
    pub sha256: String,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct FoundationArtifactVerification {
    pub path: PathBuf,
    pub width: u32,
    pub height: u32,
    /// Top/left active-sensor pixels removed to canonicalize the model input
    /// to an RGGB origin. Each component is zero or one.
    pub force_rggb_crop_sensor: [u32; 2],
    pub source_sha256: String,
    pub source_size_bytes: u64,
    pub source_pixel_contract_sha256: String,
    pub model_package_sha256: String,
    pub model_graph_sha256: String,
    pub implementation_revision: String,
    pub cache_key_sha256: String,
    pub artifact_identity_sha256: String,
    pub sequence_sha256: String,
    pub payload_bytes: u64,
    pub file_bytes: u64,
    pub file_sha256: String,
    pub stripes: Vec<FoundationArtifactStripe>,
}

/// Holds the same verified file descriptor for bounded random row reads.
#[derive(Debug)]
pub struct FoundationArtifactReader {
    file: File,
    verification: FoundationArtifactVerification,
}

impl FoundationArtifactReader {
    /// Opens and completely verifies one artifact before retaining it.
    ///
    /// # Errors
    ///
    /// Returns an error for any schema, identity, bounds, digest, pixel, or
    /// filesystem failure.
    pub fn open(path: impl AsRef<Path>) -> Result<Self, FoundationArtifactError> {
        ensure_little_endian()?;
        let path = path.as_ref().to_path_buf();
        let mut file = File::open(&path).map_err(|source| io_error(&path, source))?;
        let verification = verify_stream(&mut file, &path)?;
        Ok(Self { file, verification })
    }

    pub const fn verification(&self) -> &FoundationArtifactVerification {
        &self.verification
    }

    /// Reads a bounded row range as native-endian interleaved RGB float32.
    ///
    /// The file remains planar CHW on disk. Conversion allocates only the
    /// requested output plus one channel-sized byte buffer, even when the
    /// range crosses stripe boundaries.
    ///
    /// # Errors
    ///
    /// Returns an error when the requested range is empty, outside the image,
    /// or cannot be read from the already-verified descriptor.
    pub fn read_interleaved_rows(
        &mut self,
        y_start: u32,
        rows: u32,
    ) -> Result<Vec<f32>, FoundationArtifactError> {
        let request_end = y_start
            .checked_add(rows)
            .ok_or(FoundationArtifactError::Invalid(
                "row request overflows its coordinate space",
            ))?;
        if rows == 0 || request_end > self.verification.height {
            return Err(FoundationArtifactError::Invalid(
                "row request is outside the foundation artifact",
            ));
        }
        let width = usize::try_from(self.verification.width).map_err(|_| {
            FoundationArtifactError::Invalid("foundation width does not fit memory")
        })?;
        let row_count = usize::try_from(rows).map_err(|_| {
            FoundationArtifactError::Invalid("foundation row count does not fit memory")
        })?;
        let output_values = row_count
            .checked_mul(width)
            .and_then(|value| value.checked_mul(3))
            .ok_or(FoundationArtifactError::Invalid(
                "foundation row allocation overflows",
            ))?;
        let mut output = vec![0.0_f32; output_values];

        for stripe in &self.verification.stripes {
            let stripe_end = stripe.y_start + stripe.rows;
            let overlap_start = y_start.max(stripe.y_start);
            let overlap_end = request_end.min(stripe_end);
            if overlap_start >= overlap_end {
                continue;
            }
            let local_start = u64::from(overlap_start - stripe.y_start);
            let destination_start = usize::try_from(overlap_start - y_start).map_err(|_| {
                FoundationArtifactError::Invalid("row destination does not fit memory")
            })?;
            let overlap_rows = usize::try_from(overlap_end - overlap_start)
                .map_err(|_| FoundationArtifactError::Invalid("row overlap does not fit memory"))?;
            let row_bytes = width.checked_mul(std::mem::size_of::<f32>()).ok_or(
                FoundationArtifactError::Invalid("foundation row byte count overflows"),
            )?;
            let channel_bytes = u64::from(stripe.rows)
                .checked_mul(u64::try_from(row_bytes).map_err(|_| {
                    FoundationArtifactError::Invalid("channel byte count does not fit the file")
                })?)
                .ok_or(FoundationArtifactError::Invalid(
                    "foundation channel byte count overflows",
                ))?;
            let read_bytes =
                overlap_rows
                    .checked_mul(row_bytes)
                    .ok_or(FoundationArtifactError::Invalid(
                        "foundation row read allocation overflows",
                    ))?;
            let mut channel_payload = vec![0_u8; read_bytes];

            for channel in 0_usize..3 {
                let channel_file_index = u64::try_from(channel).map_err(|_| {
                    FoundationArtifactError::Invalid("channel index does not fit the file")
                })?;
                let offset = stripe
                    .offset
                    .checked_add(channel_file_index.checked_mul(channel_bytes).ok_or(
                        FoundationArtifactError::Invalid("foundation channel offset overflows"),
                    )?)
                    .and_then(|value| value.checked_add(local_start * row_bytes as u64))
                    .ok_or(FoundationArtifactError::Invalid(
                        "foundation row offset overflows",
                    ))?;
                seek(&mut self.file, &self.verification.path, offset)?;
                read_exact(
                    &mut self.file,
                    &self.verification.path,
                    &mut channel_payload,
                )?;
                for (sample_index, sample) in channel_payload.chunks_exact(4).enumerate() {
                    let value = f32::from_le_bytes([sample[0], sample[1], sample[2], sample[3]]);
                    let source_row = sample_index / width;
                    let x = sample_index % width;
                    let destination = ((destination_start + source_row) * width + x) * 3 + channel;
                    output[destination] = value;
                }
            }
        }
        Ok(output)
    }
}

/// Completely verifies one `.shadowrawf` without retaining its descriptor.
///
/// # Errors
///
/// Returns an error for any schema, identity, bounds, digest, pixel, or
/// filesystem failure.
pub fn verify_foundation_artifact(
    path: impl AsRef<Path>,
) -> Result<FoundationArtifactVerification, FoundationArtifactError> {
    ensure_little_endian()?;
    let path = path.as_ref().to_path_buf();
    let mut file = File::open(&path).map_err(|source| io_error(&path, source))?;
    verify_stream(&mut file, &path)
}

/// Computes the lowercase SHA-256 identity of one source file.
///
/// Foundation planning uses the complete source bytes rather than a mutable
/// path, timestamp, or fast fingerprint. Keeping this streaming implementation
/// beside artifact verification ensures both identities use the same checked
/// digest implementation without loading a RAW into memory.
///
/// # Errors
///
/// Returns an I/O error when the source cannot be opened, read, or rewound.
pub fn sha256_file(path: impl AsRef<Path>) -> Result<String, FoundationArtifactError> {
    let path = path.as_ref().to_path_buf();
    let mut file = File::open(&path).map_err(|source| io_error(&path, source))?;
    hash_file(&mut file, &path)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct FoundationAlgorithm {
    blend_overlap_packed: u32,
    blend_width_packed: u32,
    exact_halo_packed: u32,
    implementation_revision: String,
    inference_passes: u32,
    input_channel_order: [String; 4],
    normalization: String,
    output_scale: u32,
    output_space: String,
    padding: String,
    pool_alignment_packed: u32,
    scale_policy: String,
    step_packed: u32,
    tile_edge_packed: u32,
    white_balance: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct FoundationExecution {
    active_providers: Vec<String>,
    engine: String,
    machine: String,
    platform: String,
    requested_provider: String,
    runtime_version: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct FoundationModel {
    graph_member: String,
    graph_sha256: String,
    license: String,
    package_sha256: String,
    release: String,
    repository: String,
    revision: String,
    training_repository: String,
    training_revision: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct RawPreprocessing {
    black_level_per_channel: Vec<f64>,
    color_description: String,
    force_rggb_crop_sensor: [u32; 2],
    packed_shape: [u64; 3],
    raw_pattern: [[i64; 2]; 2],
    sensor_shape: [u64; 2],
    source_raw_pattern: [[i64; 2]; 2],
    white_level: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct FoundationContract {
    algorithm: FoundationAlgorithm,
    execution: FoundationExecution,
    model: FoundationModel,
    raw_preprocessing: RawPreprocessing,
    source_pixel_contract_sha256: String,
    source_sha256: String,
    source_size_bytes: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct FoundationOutput {
    byte_order: String,
    channels: [String; 3],
    demosaiced: bool,
    dtype: String,
    layout: String,
    shape_sensor: [u64; 3],
    space: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct FoundationHeader {
    cache_key_sha256: String,
    contract: FoundationContract,
    output: FoundationOutput,
    schema: String,
    semantic_boundary: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct StripePublication {
    first_pass_raw_output_mean: f64,
    global_gain: f64,
    global_input_mean: f64,
    output_mean: f64,
    producer: String,
    replay_relative_mean_delta: f64,
    second_pass_raw_output_mean: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct StripeEntry {
    byte_length: u64,
    index: usize,
    offset: u64,
    rows: u32,
    sha256: String,
    y_start: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct FoundationManifest {
    artifact_identity_sha256: String,
    cache_key_sha256: String,
    header_sha256: String,
    payload_bytes: u64,
    publication: StripePublication,
    schema: String,
    sequence_sha256: String,
    stripes: Vec<StripeEntry>,
}

#[derive(Serialize)]
struct CacheKeyMaterial<'a> {
    contract: &'a FoundationContract,
    output_shape_sensor: [u64; 3],
    schema: &'static str,
}

#[derive(Serialize)]
struct ArtifactIdentityMaterial<'a> {
    cache_key_sha256: &'a str,
    header_sha256: &'a str,
    payload_bytes: u64,
    publication: &'a StripePublication,
    schema: &'a str,
    sequence_sha256: &'a str,
    stripes: &'a [StripeEntry],
}

#[derive(Serialize)]
struct SequenceHeader {
    format: &'static str,
    shape: [u64; 3],
}

struct ParsedArtifact {
    file_bytes: u64,
    header: FoundationHeader,
    header_bytes: Vec<u8>,
    payload_start: u64,
    manifest_offset: u64,
    manifest: FoundationManifest,
}

struct VerifiedPayload {
    width: u32,
    height: u32,
    payload_bytes: u64,
    stripes: Vec<FoundationArtifactStripe>,
}

fn verify_stream(
    file: &mut File,
    path: &Path,
) -> Result<FoundationArtifactVerification, FoundationArtifactError> {
    let parsed = read_artifact_framing(file, path)?;
    validate_manifest_identity(&parsed.header, &parsed.header_bytes, &parsed.manifest)?;
    let payload = verify_artifact_payload(
        file,
        path,
        &parsed.header,
        &parsed.manifest,
        parsed.payload_start,
        parsed.manifest_offset,
    )?;
    let file_sha256 = hash_file(file, path)?;

    Ok(FoundationArtifactVerification {
        path: path.canonicalize().unwrap_or_else(|_| path.to_path_buf()),
        width: payload.width,
        height: payload.height,
        force_rggb_crop_sensor: parsed
            .header
            .contract
            .raw_preprocessing
            .force_rggb_crop_sensor,
        source_sha256: parsed.header.contract.source_sha256,
        source_size_bytes: parsed.header.contract.source_size_bytes,
        source_pixel_contract_sha256: parsed.header.contract.source_pixel_contract_sha256,
        model_package_sha256: parsed.header.contract.model.package_sha256,
        model_graph_sha256: parsed.header.contract.model.graph_sha256,
        implementation_revision: parsed.header.contract.algorithm.implementation_revision,
        cache_key_sha256: parsed.manifest.cache_key_sha256,
        artifact_identity_sha256: parsed.manifest.artifact_identity_sha256,
        sequence_sha256: parsed.manifest.sequence_sha256,
        payload_bytes: payload.payload_bytes,
        file_bytes: parsed.file_bytes,
        file_sha256,
        stripes: payload.stripes,
    })
}

fn read_artifact_framing(
    file: &mut File,
    path: &Path,
) -> Result<ParsedArtifact, FoundationArtifactError> {
    let file_bytes = file
        .metadata()
        .map_err(|source| io_error(path, source))?
        .len();
    if file_bytes < HEADER_PREFIX_FILE_BYTES + FOOTER_FILE_BYTES {
        return Err(FoundationArtifactError::Invalid(
            "file is smaller than its fixed framing",
        ));
    }

    seek(file, path, 0)?;
    let mut header_prefix = [0_u8; HEADER_PREFIX_BYTES];
    read_exact(file, path, &mut header_prefix)?;
    if &header_prefix[..8] != FILE_MAGIC {
        return Err(FoundationArtifactError::Invalid("file magic changed"));
    }
    let header_length = u64::from_le_bytes(
        header_prefix[8..16]
            .try_into()
            .expect("fixed header-length bytes"),
    );
    if header_length == 0 || header_length > MAX_JSON_BYTES {
        return Err(FoundationArtifactError::Invalid(
            "header length is outside the format bound",
        ));
    }
    let mut header_bytes = vec![
        0_u8;
        usize::try_from(header_length).map_err(|_| {
            FoundationArtifactError::Invalid("header length does not fit memory")
        })?
    ];
    read_exact(file, path, &mut header_bytes)?;
    let header: FoundationHeader =
        serde_json::from_slice(&header_bytes).map_err(|source| FoundationArtifactError::Json {
            part: "header",
            source,
        })?;
    validate_header(&header)?;

    seek(file, path, file_bytes - FOOTER_FILE_BYTES)?;
    let mut footer = [0_u8; FOOTER_BYTES];
    read_exact(file, path, &mut footer)?;
    if &footer[..8] != FOOTER_MAGIC {
        return Err(FoundationArtifactError::Invalid("footer magic changed"));
    }
    let manifest_offset =
        u64::from_le_bytes(footer[8..16].try_into().expect("manifest offset bytes"));
    let manifest_length =
        u64::from_le_bytes(footer[16..24].try_into().expect("manifest length bytes"));
    let manifest_digest: [u8; 32] = footer[24..56].try_into().expect("manifest digest bytes");
    let payload_start = HEADER_PREFIX_FILE_BYTES + header_length;
    if manifest_length == 0
        || manifest_length > MAX_JSON_BYTES
        || manifest_offset < payload_start
        || manifest_offset
            .checked_add(manifest_length)
            .and_then(|value| value.checked_add(FOOTER_FILE_BYTES))
            != Some(file_bytes)
    {
        return Err(FoundationArtifactError::Invalid(
            "manifest bounds do not exactly close the file",
        ));
    }
    seek(file, path, manifest_offset)?;
    let mut manifest_bytes = vec![
        0_u8;
        usize::try_from(manifest_length).map_err(|_| {
            FoundationArtifactError::Invalid("manifest length does not fit memory")
        })?
    ];
    read_exact(file, path, &mut manifest_bytes)?;
    if sha256_bytes(&manifest_bytes) != manifest_digest {
        return Err(FoundationArtifactError::Invalid(
            "manifest digest does not match its footer",
        ));
    }
    let manifest: FoundationManifest =
        serde_json::from_slice(&manifest_bytes).map_err(|source| {
            FoundationArtifactError::Json {
                part: "manifest",
                source,
            }
        })?;

    Ok(ParsedArtifact {
        file_bytes,
        header,
        header_bytes,
        payload_start,
        manifest_offset,
        manifest,
    })
}

fn validate_manifest_identity(
    header: &FoundationHeader,
    header_bytes: &[u8],
    manifest: &FoundationManifest,
) -> Result<(), FoundationArtifactError> {
    validate_sha256(&manifest.artifact_identity_sha256)?;
    validate_sha256(&manifest.cache_key_sha256)?;
    validate_sha256(&manifest.header_sha256)?;
    validate_sha256(&manifest.sequence_sha256)?;
    if manifest.schema != ARTIFACT_SCHEMA {
        return Err(FoundationArtifactError::Invalid(
            "manifest schema is unsupported",
        ));
    }
    if manifest.cache_key_sha256 != header.cache_key_sha256 {
        return Err(FoundationArtifactError::Invalid(
            "manifest and header cache keys differ",
        ));
    }
    if manifest.header_sha256 != hex_digest(sha256_bytes(header_bytes)) {
        return Err(FoundationArtifactError::Invalid(
            "header digest does not match the manifest",
        ));
    }
    validate_publication(&manifest.publication)?;
    let artifact_identity = canonical_sha256(&ArtifactIdentityMaterial {
        cache_key_sha256: &manifest.cache_key_sha256,
        header_sha256: &manifest.header_sha256,
        payload_bytes: manifest.payload_bytes,
        publication: &manifest.publication,
        schema: &manifest.schema,
        sequence_sha256: &manifest.sequence_sha256,
        stripes: &manifest.stripes,
    })?;
    if manifest.artifact_identity_sha256 != artifact_identity {
        return Err(FoundationArtifactError::IdentityMismatch {
            recorded: manifest.artifact_identity_sha256.clone(),
            computed: artifact_identity,
        });
    }
    Ok(())
}

fn verify_artifact_payload(
    file: &mut File,
    path: &Path,
    header: &FoundationHeader,
    manifest: &FoundationManifest,
    payload_start: u64,
    manifest_offset: u64,
) -> Result<VerifiedPayload, FoundationArtifactError> {
    let height = u32::try_from(header.output.shape_sensor[1]).map_err(|_| {
        FoundationArtifactError::Invalid("foundation height exceeds the supported range")
    })?;
    let width = u32::try_from(header.output.shape_sensor[2]).map_err(|_| {
        FoundationArtifactError::Invalid("foundation width exceeds the supported range")
    })?;
    if manifest.stripes.is_empty()
        || manifest.stripes.len()
            > usize::try_from(height).map_err(|_| {
                FoundationArtifactError::Invalid("foundation height does not fit memory")
            })?
    {
        return Err(FoundationArtifactError::Invalid(
            "stripe table is empty or exceeds image height",
        ));
    }

    let sequence_header = canonical_json(&SequenceHeader {
        format: SEQUENCE_FORMAT,
        shape: header.output.shape_sensor,
    })?;
    let mut sequence_hasher = Sha256::new();
    sequence_hasher.update(&sequence_header);
    let mut stripes = Vec::with_capacity(manifest.stripes.len());
    let mut cursor = payload_start;
    let mut next_y = 0_u32;
    let mut payload_bytes = 0_u64;
    let row_bytes = u64::from(width)
        .checked_mul(12)
        .ok_or(FoundationArtifactError::Invalid(
            "foundation row byte count overflows",
        ))?;

    for (index, stripe) in manifest.stripes.iter().enumerate() {
        validate_sha256(&stripe.sha256)?;
        if stripe.index != index || stripe.y_start != next_y {
            return Err(FoundationArtifactError::Invalid(
                "stripes are not indexed and contiguous",
            ));
        }
        let stripe_end =
            next_y
                .checked_add(stripe.rows)
                .ok_or(FoundationArtifactError::Invalid(
                    "stripe row geometry overflows",
                ))?;
        let expected_bytes = u64::from(stripe.rows).checked_mul(row_bytes).ok_or(
            FoundationArtifactError::Invalid("stripe byte length overflows"),
        )?;
        if stripe.rows == 0
            || stripe_end > height
            || stripe.offset != cursor
            || stripe.byte_length != expected_bytes
        {
            return Err(FoundationArtifactError::Invalid(
                "stripe geometry or byte length is invalid",
            ));
        }
        verify_stripe_bytes(file, path, stripe, &mut sequence_hasher)?;
        stripes.push(FoundationArtifactStripe {
            index,
            y_start: stripe.y_start,
            rows: stripe.rows,
            offset: stripe.offset,
            byte_length: stripe.byte_length,
            sha256: stripe.sha256.clone(),
        });
        next_y = stripe_end;
        cursor = cursor
            .checked_add(stripe.byte_length)
            .ok_or(FoundationArtifactError::Invalid("payload cursor overflows"))?;
        payload_bytes = payload_bytes.checked_add(stripe.byte_length).ok_or(
            FoundationArtifactError::Invalid("payload byte total overflows"),
        )?;
    }
    if next_y != height || cursor != manifest_offset {
        return Err(FoundationArtifactError::Invalid(
            "stripes do not cover the complete artifact payload",
        ));
    }
    if payload_bytes != manifest.payload_bytes {
        return Err(FoundationArtifactError::Invalid(
            "payload byte count does not match the manifest",
        ));
    }
    if manifest.sequence_sha256 != hex_digest(sequence_hasher.finalize()) {
        return Err(FoundationArtifactError::Invalid(
            "ordered stripe sequence digest does not match",
        ));
    }
    Ok(VerifiedPayload {
        width,
        height,
        payload_bytes,
        stripes,
    })
}

fn verify_stripe_bytes(
    file: &mut File,
    path: &Path,
    stripe: &StripeEntry,
    sequence_hasher: &mut Sha256,
) -> Result<(), FoundationArtifactError> {
    seek(file, path, stripe.offset)?;
    let mut stripe_hasher = Sha256::new();
    let mut remaining = stripe.byte_length;
    let mut buffer = vec![0_u8; HASH_CHUNK_BYTES];
    while remaining > 0 {
        let count =
            usize::try_from(remaining.min(HASH_CHUNK_BYTES as u64)).expect("bounded hash chunk");
        read_exact(file, path, &mut buffer[..count])?;
        if !buffer[..count].chunks_exact(4).all(|sample| {
            f32::from_le_bytes([sample[0], sample[1], sample[2], sample[3]]).is_finite()
        }) {
            return Err(FoundationArtifactError::Invalid(
                "stripe payload contains a non-finite float",
            ));
        }
        stripe_hasher.update(&buffer[..count]);
        sequence_hasher.update(&buffer[..count]);
        remaining -= count as u64;
    }
    if stripe.sha256 != hex_digest(stripe_hasher.finalize()) {
        return Err(FoundationArtifactError::Invalid(
            "stripe payload digest does not match the manifest",
        ));
    }
    Ok(())
}

fn validate_header(header: &FoundationHeader) -> Result<(), FoundationArtifactError> {
    if header.schema != HEADER_SCHEMA
        || header.semantic_boundary != "raw-foundation-materialization"
    {
        return Err(FoundationArtifactError::Invalid(
            "header schema or semantic boundary is unsupported",
        ));
    }
    let output = &header.output;
    if output.byte_order != "little"
        || output.channels != ["R", "G", "B"]
        || !output.demosaiced
        || output.dtype != "float32"
        || output.layout != "stripe-chw"
        || output.space != "linear-camera-rgb"
        || output.shape_sensor[0] != 3
        || output.shape_sensor[1] == 0
        || output.shape_sensor[2] == 0
        || output.shape_sensor[1..] != header.contract.raw_preprocessing.sensor_shape
    {
        return Err(FoundationArtifactError::Invalid(
            "output pixel contract is unsupported",
        ));
    }
    validate_contract(&header.contract)?;
    validate_sha256(&header.cache_key_sha256)?;
    let expected_cache_key = canonical_sha256(&CacheKeyMaterial {
        contract: &header.contract,
        output_shape_sensor: output.shape_sensor,
        schema: CACHE_KEY_SCHEMA,
    })?;
    if header.cache_key_sha256 != expected_cache_key {
        return Err(FoundationArtifactError::Invalid(
            "header cache key does not match its contract",
        ));
    }
    Ok(())
}

fn validate_contract(contract: &FoundationContract) -> Result<(), FoundationArtifactError> {
    validate_sha256(&contract.source_sha256)?;
    validate_sha256(&contract.source_pixel_contract_sha256)?;
    if contract.source_pixel_contract_sha256 != SOURCE_PIXEL_CONTRACT_SHA256 {
        return Err(FoundationArtifactError::Invalid(
            "source pixel contract is unsupported",
        ));
    }
    if contract.source_size_bytes == 0 {
        return Err(FoundationArtifactError::Invalid(
            "source size must be positive",
        ));
    }
    let preprocessing = &contract.raw_preprocessing;
    if preprocessing.packed_shape[0] != 4
        || preprocessing.sensor_shape
            != [
                preprocessing.packed_shape[1] * 2,
                preprocessing.packed_shape[2] * 2,
            ]
        || preprocessing
            .force_rggb_crop_sensor
            .iter()
            .any(|value| *value > 1)
        || preprocessing.color_description.is_empty()
        || preprocessing.black_level_per_channel.len() < 4
        || !preprocessing.white_level.is_finite()
        || preprocessing
            .black_level_per_channel
            .iter()
            .any(|value| !value.is_finite())
    {
        return Err(FoundationArtifactError::Invalid(
            "RAW preprocessing contract is invalid",
        ));
    }
    let model = &contract.model;
    if model.graph_member != "rawdenoise-nind/model_bayer.onnx"
        || model.graph_sha256 != BAYER_GRAPH_SHA256
        || model.license != "GPL-3.0"
        || model.package_sha256 != PACKAGE_SHA256
        || model.release != UPSTREAM_RELEASE
        || model.repository != UPSTREAM_REPOSITORY
        || model.revision != UPSTREAM_REVISION
        || model.training_repository != TRAINING_REPOSITORY
        || model.training_revision != TRAINING_REVISION
    {
        return Err(FoundationArtifactError::Invalid(
            "public model identity is unsupported",
        ));
    }
    let execution = &contract.execution;
    if execution.engine.is_empty()
        || execution.runtime_version.is_empty()
        || execution.requested_provider.is_empty()
        || execution.active_providers.is_empty()
        || execution.active_providers.iter().any(String::is_empty)
        || execution.platform.is_empty()
        || execution.machine.is_empty()
    {
        return Err(FoundationArtifactError::Invalid(
            "execution identity is incomplete",
        ));
    }
    let algorithm = &contract.algorithm;
    if algorithm.implementation_revision != IMPLEMENTATION_REVISION
        || algorithm.inference_passes != 2
        || algorithm.input_channel_order != ["R", "G1", "G2", "B"]
        || algorithm.normalization != "per-cfa-site-black-to-white-range-clipped"
        || algorithm.output_scale != 2
        || algorithm.output_space != "linear-camera-rgb"
        || algorithm.padding != "numpy-reflect-direct-index"
        || algorithm.pool_alignment_packed != 16
        || algorithm.scale_policy != "one-global-output-mean-to-input-mean"
        || algorithm.tile_edge_packed != 512
        || algorithm.white_balance != "none"
        || algorithm.blend_overlap_packed <= algorithm.exact_halo_packed
        || algorithm.blend_width_packed
            != 2 * (algorithm.blend_overlap_packed - algorithm.exact_halo_packed)
        || algorithm.step_packed != algorithm.tile_edge_packed - 2 * algorithm.blend_overlap_packed
        || algorithm.step_packed == 0
        || !algorithm.step_packed.is_multiple_of(16)
    {
        return Err(FoundationArtifactError::Invalid(
            "foundation algorithm identity is unsupported",
        ));
    }
    Ok(())
}

fn validate_publication(publication: &StripePublication) -> Result<(), FoundationArtifactError> {
    let values = [
        publication.global_input_mean,
        publication.first_pass_raw_output_mean,
        publication.second_pass_raw_output_mean,
        publication.replay_relative_mean_delta,
        publication.global_gain,
        publication.output_mean,
    ];
    if publication.producer != PUBLICATION_PRODUCER
        || values.iter().any(|value| !value.is_finite())
        || publication.replay_relative_mean_delta < 0.0
    {
        return Err(FoundationArtifactError::Invalid(
            "stripe publication is invalid",
        ));
    }
    let raw_scale = 0.5
        * (publication.first_pass_raw_output_mean.abs()
            + publication.second_pass_raw_output_mean.abs());
    let expected_delta = if raw_scale == 0.0 {
        0.0
    } else {
        (publication.first_pass_raw_output_mean - publication.second_pass_raw_output_mean).abs()
            / raw_scale
    };
    if !close(
        publication.replay_relative_mean_delta,
        expected_delta,
        1e-12,
        1e-15,
    ) || publication.first_pass_raw_output_mean.abs()
        <= 1e-6 * publication.global_input_mean.abs()
    {
        return Err(FoundationArtifactError::Invalid(
            "stripe replay receipt is inconsistent",
        ));
    }
    let expected_gain = publication.global_input_mean / publication.first_pass_raw_output_mean;
    let input_scale = publication.global_input_mean.abs().max(1e-12);
    if !close(publication.global_gain, expected_gain, 1e-12, 0.0)
        || (publication.output_mean - publication.global_input_mean).abs() / input_scale > 1e-6
    {
        return Err(FoundationArtifactError::Invalid(
            "stripe gain receipt is inconsistent",
        ));
    }
    Ok(())
}

fn close(left: f64, right: f64, relative: f64, absolute: f64) -> bool {
    (left - right).abs() <= absolute.max(relative * left.abs().max(right.abs()))
}

fn ensure_little_endian() -> Result<(), FoundationArtifactError> {
    if cfg!(target_endian = "little") {
        Ok(())
    } else {
        Err(FoundationArtifactError::Invalid(
            "reader requires a little-endian host",
        ))
    }
}

fn validate_sha256(value: &str) -> Result<(), FoundationArtifactError> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(FoundationArtifactError::Invalid(
            "SHA-256 identity is not lowercase hexadecimal",
        ));
    }
    Ok(())
}

fn canonical_json(value: &impl Serialize) -> Result<Vec<u8>, FoundationArtifactError> {
    let encoded = serde_json::to_vec(value).map_err(|source| FoundationArtifactError::Json {
        part: "canonical identity",
        source,
    })?;
    Ok(normalize_python_json_exponents(&encoded))
}

fn normalize_python_json_exponents(encoded: &[u8]) -> Vec<u8> {
    let mut normalized = Vec::with_capacity(encoded.len());
    let mut index = 0;
    let mut in_string = false;
    let mut escaped = false;
    while index < encoded.len() {
        let byte = encoded[index];
        if in_string {
            normalized.push(byte);
            if escaped {
                escaped = false;
            } else if byte == b'\\' {
                escaped = true;
            } else if byte == b'"' {
                in_string = false;
            }
            index += 1;
            continue;
        }
        if byte == b'"' {
            in_string = true;
            normalized.push(byte);
            index += 1;
            continue;
        }
        if byte == b'e' || byte == b'E' {
            normalized.push(b'e');
            index += 1;
            if index < encoded.len() && matches!(encoded[index], b'+' | b'-') {
                normalized.push(encoded[index]);
                index += 1;
            }
            let digit_start = index;
            while index < encoded.len() && encoded[index].is_ascii_digit() {
                index += 1;
            }
            if index - digit_start == 1 {
                normalized.push(b'0');
            }
            normalized.extend_from_slice(&encoded[digit_start..index]);
            continue;
        }
        normalized.push(byte);
        index += 1;
    }
    normalized
}

fn canonical_sha256(value: &impl Serialize) -> Result<String, FoundationArtifactError> {
    Ok(hex_digest(sha256_bytes(&canonical_json(value)?)))
}

fn hash_file(file: &mut File, path: &Path) -> Result<String, FoundationArtifactError> {
    seek(file, path, 0)?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0_u8; HASH_CHUNK_BYTES];
    loop {
        let count = file
            .read(&mut buffer)
            .map_err(|source| io_error(path, source))?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(hex_digest(hasher.finalize()))
}

fn read_exact(
    file: &mut File,
    path: &Path,
    value: &mut [u8],
) -> Result<(), FoundationArtifactError> {
    file.read_exact(value)
        .map_err(|source| io_error(path, source))
}

fn seek(file: &mut File, path: &Path, offset: u64) -> Result<(), FoundationArtifactError> {
    file.seek(SeekFrom::Start(offset))
        .map(|_| ())
        .map_err(|source| io_error(path, source))
}

fn io_error(path: &Path, source: io::Error) -> FoundationArtifactError {
    FoundationArtifactError::Io {
        path: path.to_path_buf(),
        source,
    }
}

fn sha256_bytes(value: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(value);
    hasher.finalize()
}

fn hex_digest(value: [u8; 32]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut result = String::with_capacity(64);
    for byte in value {
        result.push(char::from(HEX[usize::from(byte >> 4)]));
        result.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    result
}

struct Sha256 {
    state: [u32; 8],
    buffer: [u8; 64],
    buffer_len: usize,
    byte_len: u64,
}

impl Sha256 {
    #[allow(clippy::unreadable_literal)] // Values are copied from the SHA-256 specification.
    const fn new() -> Self {
        Self {
            state: [
                0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
                0x5be0cd19,
            ],
            buffer: [0; 64],
            buffer_len: 0,
            byte_len: 0,
        }
    }

    fn update(&mut self, mut value: &[u8]) {
        self.byte_len = self
            .byte_len
            .checked_add(value.len() as u64)
            .expect("foundation SHA-256 input is bounded by file size");
        if self.buffer_len != 0 {
            let count = (64 - self.buffer_len).min(value.len());
            self.buffer[self.buffer_len..self.buffer_len + count].copy_from_slice(&value[..count]);
            self.buffer_len += count;
            value = &value[count..];
            if self.buffer_len == 64 {
                let block = self.buffer;
                self.compress(&block);
                self.buffer_len = 0;
            }
        }
        while value.len() >= 64 {
            let block: &[u8; 64] = value[..64].try_into().expect("complete SHA-256 block");
            self.compress(block);
            value = &value[64..];
        }
        self.buffer[..value.len()].copy_from_slice(value);
        self.buffer_len = value.len();
    }

    fn finalize(mut self) -> [u8; 32] {
        let bit_len = self.byte_len.checked_mul(8).expect("SHA-256 bit length");
        self.buffer[self.buffer_len] = 0x80;
        self.buffer_len += 1;
        if self.buffer_len > 56 {
            self.buffer[self.buffer_len..].fill(0);
            let block = self.buffer;
            self.compress(&block);
            self.buffer = [0; 64];
        } else {
            self.buffer[self.buffer_len..56].fill(0);
        }
        self.buffer[56..64].copy_from_slice(&bit_len.to_be_bytes());
        let block = self.buffer;
        self.compress(&block);

        let mut output = [0_u8; 32];
        for (chunk, word) in output.chunks_exact_mut(4).zip(self.state) {
            chunk.copy_from_slice(&word.to_be_bytes());
        }
        output
    }

    #[allow(
        clippy::many_single_char_names,
        clippy::unreadable_literal // Values are copied from the SHA-256 specification.
    )]
    fn compress(&mut self, block: &[u8; 64]) {
        const K: [u32; 64] = [
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2,
        ];
        let mut words = [0_u32; 64];
        for (index, chunk) in block.chunks_exact(4).enumerate() {
            words[index] = u32::from_be_bytes(chunk.try_into().expect("four-byte SHA word"));
        }
        for index in 16..64 {
            let s0 = words[index - 15].rotate_right(7)
                ^ words[index - 15].rotate_right(18)
                ^ (words[index - 15] >> 3);
            let s1 = words[index - 2].rotate_right(17)
                ^ words[index - 2].rotate_right(19)
                ^ (words[index - 2] >> 10);
            words[index] = words[index - 16]
                .wrapping_add(s0)
                .wrapping_add(words[index - 7])
                .wrapping_add(s1);
        }

        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = self.state;
        for index in 0..64 {
            let sum1 = (e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25))
                .wrapping_add((e & f) ^ ((!e) & g))
                .wrapping_add(h)
                .wrapping_add(K[index])
                .wrapping_add(words[index]);
            let sum0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let majority = (a & b) ^ (a & c) ^ (b & c);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(sum1);
            d = c;
            c = b;
            b = a;
            a = sum1.wrapping_add(sum0).wrapping_add(majority);
        }
        for (state, value) in self.state.iter_mut().zip([a, b, c, d, e, f, g, h]) {
            *state = state.wrapping_add(value);
        }
    }
}

#[cfg(test)]
mod tests;
