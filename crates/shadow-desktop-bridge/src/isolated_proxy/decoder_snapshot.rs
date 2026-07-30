//! Descriptor-only decoder inspection protocol, cache, and workflow.

use std::{
    fs, io,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use shadow_domain::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot,
    ImageDimensions, PendingCorrectionsSnapshot, PreviewCodec, PreviewDescriptorSnapshot,
    RawDevelopmentCapabilitySnapshot,
};
use uuid::Uuid;

use super::{
    helper_process::{
        HELPER_TIMEOUT, HelperExecution, HelperProcessOutput, execute_decode_helper,
        helper_exit_looks_like_crash, helper_stderr_suffix,
    },
    helper_wire_fields::{decode_protocol_hex_text_field, parse_metadata_u32, parse_metadata_u64},
    metadata::parse_raw_metadata_snapshot_fields,
    persistent_evidence::{
        DecoderSafetyRegistry, IsolatedDecodeObservation, write_atomic_cache_record,
    },
    route_identity::{DECODER_SNAPSHOT_PROTOCOL, DecoderSafetyKey},
};

const MAX_DECODER_SNAPSHOT_IDENTITY_TEXT_BYTES: usize = 4 * 1024;
const MAX_DECODER_SNAPSHOT_METADATA_TEXT_BYTES: usize = 1024;
const MAX_DECODER_SNAPSHOT_PREVIEWS: usize = 64;
const DECODER_SNAPSHOT_SCHEMA: u8 = 1;
const DECODER_SNAPSHOT_CAPABILITY_FIELD_COUNT: usize = 16;
pub(super) const DECODER_SNAPSHOT_BASE_FIELD_COUNT: usize = 63;
pub(super) const DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT: usize = 8;

/// A child-established, descriptor-only decoder inspection. The nested
/// `snapshot` intentionally describes the helper-side router; callers that
/// store it through a host `DecodeInspector` must normalize the cache-facing
/// provider id/version first. The helper can load a private provider which the
/// desktop host deliberately never links.
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub(crate) struct IsolatedPhotoDecoderSnapshot {
    pub(crate) router_provider_id: String,
    pub(crate) router_provider_version: String,
    pub(crate) snapshot: DecoderSnapshot,
}

impl IsolatedPhotoDecoderSnapshot {
    /// Produces the catalog-facing snapshot for the desktop router without
    /// pretending that the helper's provider graph is in-process. This is a
    /// **catalog-only** snapshot: metadata survives, but every capability that
    /// would let a caller reopen/decode/develop source pixels in the desktop
    /// process is explicitly unavailable. The generated helper proxy remains
    /// the only safe visual path for this route.
    pub(crate) fn into_catalog_snapshot(
        mut self,
        provider_id: &str,
        provider_version: &str,
    ) -> DecoderSnapshot {
        self.snapshot.provider = DecodeProviderSnapshot {
            id: provider_id.to_owned(),
            version: provider_version.to_owned(),
            // The child established its own provider graph, not an in-process
            // claim about the host router. Do not expose helper implementation
            // flags after normalizing the identity.
            dng_sdk: false,
            rawspeed: false,
            jpeg: false,
        };
        self.snapshot.capabilities = DecodeCapabilitySnapshot {
            // The copied snapshot contains metadata, so catalog projection may
            // safely consume it without reopening the RAW.
            metadata: DecodeSupport::Available,
            embedded_previews: DecodeSupport::Unavailable,
            raw_frame: DecodeSupport::Unavailable,
            reference_rgb: DecodeSupport::Unavailable,
            // DNG opcode presence is source metadata, not a permission for
            // the host to execute those opcodes.
            pending_corrections: self.snapshot.capabilities.pending_corrections,
            raw_development: RawDevelopmentCapabilitySnapshot::default(),
        };
        self.snapshot.previews.clear();
        self.snapshot
    }
}

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
struct PersistedDecoderSnapshot {
    schema: u8,
    key: DecoderSafetyKey,
    snapshot: IsolatedPhotoDecoderSnapshot,
}

/// Opens a RAW/provider source entirely in the helper and copies the bounded
/// descriptor snapshot needed by the catalog worker. No preview bytes,
/// `RawFrame`, source path, or developed RGB crosses this protocol. A caller
/// must treat successful completion as evidence about this one inspection
/// stage—not permission to reopen the RAW in the desktop process.
pub(crate) fn snapshot_isolated_photo_decoder(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
) -> Result<IsolatedPhotoDecoderSnapshot> {
    let safety_key = DecoderSafetyKey::decoder_snapshot(source_path, helper_path)?;
    if let Some(observation) =
        DecoderSafetyRegistry::shared().observation(runtime_cache_root, &safety_key)?
        && observation.quarantines_main_process_native_decode()
    {
        bail!(
            "isolated RAW decoder inspection is quarantined for this unchanged source after {}; change the source or helper before retrying",
            observation.diagnostic_label()
        );
    }
    if let Some(snapshot) = read_decoder_snapshot(runtime_cache_root, &safety_key)? {
        return Ok(snapshot);
    }

    let nonce = Uuid::now_v7().simple().to_string();
    let execution = execute_decode_helper(helper_path, |command| {
        command.arg("decoder-snapshot").arg(source_path).arg(&nonce);
    });
    let (result, observation) = match execution {
        Ok(HelperExecution::TimedOut(output)) => (
            Err(anyhow::anyhow!(
                "isolated RAW decoder inspection timed out after {} seconds{}",
                HELPER_TIMEOUT.as_secs(),
                helper_stderr_suffix(&output.stderr)
            )),
            IsolatedDecodeObservation::TimedOut,
        ),
        Ok(HelperExecution::Completed(output)) => {
            let result = decode_decoder_snapshot_output(&output, &nonce);
            let observation = if result.is_ok() {
                IsolatedDecodeObservation::LegacyStageSucceededUnproven
            } else if !output.status.success() && helper_exit_looks_like_crash(output.status) {
                IsolatedDecodeObservation::ChildCrashed
            } else if !output.status.success() {
                IsolatedDecodeObservation::ChildRejected
            } else {
                IsolatedDecodeObservation::InvalidOutput
            };
            (result, observation)
        }
        Err(error) => (Err(error), IsolatedDecodeObservation::LaunchFailed),
    };

    let source_is_current = safety_key.is_current(source_path, helper_path);
    if source_is_current {
        // Cache publication is advisory. Preserve the actual helper result if
        // a cache volume becomes unavailable between decode and write.
        let _ =
            DecoderSafetyRegistry::shared().record(runtime_cache_root, &safety_key, observation);
        if let Ok(snapshot) = &result {
            let _ = write_decoder_snapshot(runtime_cache_root, &safety_key, snapshot);
        }
    }
    if result.is_ok() && !source_is_current {
        bail!("source or helper changed while the isolated decoder inspection was running");
    }
    result
}

fn decoder_snapshot_path(runtime_cache_root: &Path, key: &DecoderSafetyKey) -> Result<PathBuf> {
    let key = serde_json::to_vec(key).context("serialize isolated decoder snapshot key")?;
    let digest = blake3::hash(&key).to_hex();
    Ok(runtime_cache_root
        .join("decode-helper")
        .join("decoder-snapshots")
        .join(format!("{digest}.json")))
}

fn read_decoder_snapshot(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
) -> Result<Option<IsolatedPhotoDecoderSnapshot>> {
    let path = decoder_snapshot_path(runtime_cache_root, key)?;
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        // This is an acceleration/provenance cache only. A bad record must
        // schedule a fresh isolated read, never become a source identity fact.
        Err(_) => return Ok(None),
    };
    let record: PersistedDecoderSnapshot = match serde_json::from_slice(&bytes) {
        Ok(record) => record,
        Err(_) => return Ok(None),
    };
    if record.schema != DECODER_SNAPSHOT_SCHEMA || record.key != *key {
        return Ok(None);
    }
    Ok(Some(record.snapshot))
}

fn write_decoder_snapshot(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
    snapshot: &IsolatedPhotoDecoderSnapshot,
) -> Result<()> {
    let record = PersistedDecoderSnapshot {
        schema: DECODER_SNAPSHOT_SCHEMA,
        key: key.clone(),
        snapshot: snapshot.clone(),
    };
    let bytes = serde_json::to_vec(&record).context("serialize isolated decoder snapshot")?;
    let path = decoder_snapshot_path(runtime_cache_root, key)?;
    write_atomic_cache_record(&path, &bytes)
}

fn decode_decoder_snapshot_output(
    output: &HelperProcessOutput,
    nonce: &str,
) -> Result<IsolatedPhotoDecoderSnapshot> {
    if !output.status.success() {
        bail!(
            "isolated RAW decoder inspection exited with {}{}",
            output.status,
            helper_stderr_suffix(&output.stderr)
        );
    }
    parse_decoder_snapshot_protocol(&output.stdout, nonce)
}

fn parse_decoder_snapshot_protocol(
    stdout: &[u8],
    expected_nonce: &str,
) -> Result<IsolatedPhotoDecoderSnapshot> {
    if expected_nonce.is_empty() {
        bail!("isolated decoder snapshot nonce must be non-empty");
    }
    let response =
        std::str::from_utf8(stdout).context("decode isolated RAW decoder snapshot output")?;
    let fields = response.split_whitespace().collect::<Vec<_>>();
    if fields.len() < DECODER_SNAPSHOT_BASE_FIELD_COUNT
        || fields[0] != DECODER_SNAPSHOT_PROTOCOL
        || fields[1] != "decoder-snapshot"
        || fields[2] != expected_nonce
    {
        bail!("isolated RAW decoder snapshot returned an invalid protocol response");
    }

    let preview_count = usize::try_from(parse_metadata_u64(fields[62], "preview count")?)
        .context("isolated RAW decoder snapshot preview count exceeds usize")?;
    if preview_count > MAX_DECODER_SNAPSHOT_PREVIEWS {
        bail!("isolated RAW decoder snapshot exceeded the preview descriptor limit");
    }
    let expected_field_count = DECODER_SNAPSHOT_BASE_FIELD_COUNT
        .checked_add(
            preview_count
                .checked_mul(DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT)
                .ok_or_else(|| {
                    anyhow::anyhow!("isolated RAW decoder snapshot preview count overflow")
                })?,
        )
        .ok_or_else(|| anyhow::anyhow!("isolated RAW decoder snapshot field count overflow"))?;
    if fields.len() != expected_field_count {
        bail!("isolated RAW decoder snapshot returned an invalid field count");
    }

    let router_provider_id = decode_protocol_hex_text_field(
        fields[3],
        "decoder snapshot router provider id",
        MAX_DECODER_SNAPSHOT_IDENTITY_TEXT_BYTES,
        false,
    )?;
    let router_provider_version = decode_protocol_hex_text_field(
        fields[4],
        "decoder snapshot router provider version",
        MAX_DECODER_SNAPSHOT_IDENTITY_TEXT_BYTES,
        false,
    )?;
    let provider = DecodeProviderSnapshot {
        id: router_provider_id.clone(),
        version: router_provider_version.clone(),
        dng_sdk: parse_decoder_snapshot_bool(fields[5], "DNG SDK support")?,
        rawspeed: parse_decoder_snapshot_bool(fields[6], "RawSpeed support")?,
        jpeg: parse_decoder_snapshot_bool(fields[7], "JPEG support")?,
    };
    let metadata = parse_raw_metadata_snapshot_fields(
        &fields[8..46],
        MAX_DECODER_SNAPSHOT_METADATA_TEXT_BYTES,
        "isolated RAW decoder snapshot",
    )?;
    let capabilities = parse_decoder_snapshot_capabilities(&fields[46..62])?;
    let mut previews = Vec::with_capacity(preview_count);
    for index in 0..preview_count {
        let start =
            DECODER_SNAPSHOT_BASE_FIELD_COUNT + index * DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT;
        previews.push(parse_decoder_snapshot_preview(&fields[start..start + 8])?);
    }

    Ok(IsolatedPhotoDecoderSnapshot {
        router_provider_id,
        router_provider_version,
        snapshot: DecoderSnapshot {
            provider,
            metadata,
            capabilities,
            previews,
        },
    })
}

fn parse_decoder_snapshot_capabilities(fields: &[&str]) -> Result<DecodeCapabilitySnapshot> {
    if fields.len() != DECODER_SNAPSHOT_CAPABILITY_FIELD_COUNT {
        bail!("isolated RAW decoder snapshot returned invalid capabilities");
    }
    Ok(DecodeCapabilitySnapshot {
        metadata: parse_decoder_snapshot_support(fields[0], "metadata capability")?,
        embedded_previews: parse_decoder_snapshot_support(
            fields[1],
            "embedded-preview capability",
        )?,
        raw_frame: parse_decoder_snapshot_support(fields[2], "raw-frame capability")?,
        reference_rgb: parse_decoder_snapshot_support(fields[3], "reference-rgb capability")?,
        pending_corrections: PendingCorrectionsSnapshot {
            dng_opcode_list_bytes: [
                parse_metadata_u32(fields[4], "DNG opcode list 1 bytes")?,
                parse_metadata_u32(fields[5], "DNG opcode list 2 bytes")?,
                parse_metadata_u32(fields[6], "DNG opcode list 3 bytes")?,
            ],
        },
        raw_development: RawDevelopmentCapabilitySnapshot {
            plan_schema_version: parse_metadata_u32(fields[7], "RAW development schema")?,
            available: parse_decoder_snapshot_support(fields[8], "RAW development availability")?,
            raw_frame: parse_decoder_snapshot_support(fields[9], "RAW development raw-frame")?,
            dng_opcode_execution_receipt: parse_decoder_snapshot_support(
                fields[10],
                "RAW development opcode receipt",
            )?,
            supported_intents: parse_metadata_u32(fields[11], "RAW development intents")?,
            supported_qualities: parse_metadata_u32(fields[12], "RAW development qualities")?,
            supported_dng_opcode_policies: parse_metadata_u32(
                fields[13],
                "RAW development opcode policies",
            )?,
            supported_noise_reduction_intents: parse_metadata_u32(
                fields[14],
                "RAW development denoise intents",
            )?,
            supported_highlight_recovery_intents: parse_metadata_u32(
                fields[15],
                "RAW development highlight intents",
            )?,
        },
    })
}

fn parse_decoder_snapshot_preview(fields: &[&str]) -> Result<PreviewDescriptorSnapshot> {
    if fields.len() != DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT {
        bail!("isolated RAW decoder snapshot returned an invalid preview descriptor");
    }
    let provider_id = usize::try_from(parse_metadata_u64(fields[0], "preview provider id")?)
        .context("isolated RAW decoder snapshot preview provider id exceeds usize")?;
    let codec = match parse_metadata_u64(fields[1], "preview codec")? {
        0 => PreviewCodec::Unknown,
        1 => PreviewCodec::Jpeg,
        2 => PreviewCodec::Bitmap,
        3 => PreviewCodec::JpegXl,
        4 => PreviewCodec::H265,
        _ => bail!("isolated RAW decoder snapshot returned an unknown preview codec"),
    };
    Ok(PreviewDescriptorSnapshot {
        provider_id,
        codec,
        dimensions: ImageDimensions {
            width: parse_metadata_u32(fields[2], "preview width")?,
            height: parse_metadata_u32(fields[3], "preview height")?,
        },
        bits_per_channel: u16::try_from(parse_metadata_u64(fields[4], "preview bits")?)
            .context("isolated RAW decoder snapshot preview bits exceed u16")?,
        channels: u16::try_from(parse_metadata_u64(fields[5], "preview channels")?)
            .context("isolated RAW decoder snapshot preview channels exceed u16")?,
        encoded_bytes: parse_metadata_u64(fields[6], "preview encoded bytes")?,
        decodable: parse_decoder_snapshot_bool(fields[7], "preview decodable")?,
    })
}

fn parse_decoder_snapshot_support(encoded: &str, label: &str) -> Result<DecodeSupport> {
    Ok(if parse_decoder_snapshot_bool(encoded, label)? {
        DecodeSupport::Available
    } else {
        DecodeSupport::Unavailable
    })
}

fn parse_decoder_snapshot_bool(encoded: &str, label: &str) -> Result<bool> {
    match parse_metadata_u64(encoded, label)? {
        0 => Ok(false),
        1 => Ok(true),
        _ => bail!("isolated RAW decoder snapshot {label} is not a boolean"),
    }
}

#[cfg(test)]
mod tests;
