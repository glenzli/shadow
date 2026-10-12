//! Bounded, nonce-bound camera JPEG extraction in the isolated provider process.

use std::{fs, path::Path};

use anyhow::{Context, Result, bail};
use shadow_domain::{
    ImageDimensions, PreviewByteOrder, PreviewCodec, PreviewDescriptorSnapshot, PreviewPayload,
};
use uuid::Uuid;

use super::{
    helper_process::{
        HELPER_TIMEOUT, HelperExecution, HelperProcessOutput, execute_decode_helper,
        helper_exit_looks_like_crash, helper_stderr_suffix,
    },
    persistent_evidence::{
        DecoderSafetyRegistry, IsolatedDecodeObservation,
        record_safety_observation_if_source_is_current,
    },
    route_identity::{DecoderSafetyKey, EMBEDDED_PREVIEW_PROTOCOL},
};

const MAX_PREVIEW_BYTES: u64 = 64 * 1024 * 1024;
const MAX_PREVIEW_SIDE: u32 = 16_384;

pub(crate) fn extract_isolated_embedded_preview(
    helper: &Path,
    cache_root: &Path,
    source: &Path,
) -> Result<Option<PreviewPayload>> {
    let key = DecoderSafetyKey::embedded_preview(source, helper)?;
    if let Some(observation) = DecoderSafetyRegistry::shared().observation(cache_root, &key)?
        && observation.quarantines_main_process_native_decode()
    {
        bail!(
            "isolated embedded preview is quarantined after {}",
            observation.diagnostic_label()
        );
    }
    let directory = cache_root.join("decode-helper");
    fs::create_dir_all(&directory).context("create isolated embedded preview directory")?;
    let nonce = Uuid::now_v7().simple().to_string();
    let output_path = directory.join(format!("embedded-{nonce}.jpg"));
    let execution = execute_decode_helper(helper, |command| {
        command
            .arg("embedded-preview")
            .arg(source)
            .arg(&output_path)
            .arg(&nonce);
    });
    let (result, observation) = match execution {
        Ok(HelperExecution::TimedOut(output)) => (
            Err(anyhow::anyhow!(
                "isolated embedded preview timed out after {} seconds{}",
                HELPER_TIMEOUT.as_secs(),
                helper_stderr_suffix(&output.stderr)
            )),
            IsolatedDecodeObservation::TimedOut,
        ),
        Ok(HelperExecution::Completed(output)) => {
            let result = decode_output(&output, &output_path, &nonce);
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
    // Camera bytes remain session-local. Success, absence, invalid output, timeout and source
    // replacement all remove the private transfer file before returning.
    let _ = fs::remove_file(&output_path);
    record_safety_observation_if_source_is_current(cache_root, source, helper, &key, observation);
    if result.is_ok() && !key.is_current(source, helper) {
        bail!("source or helper changed during isolated embedded preview extraction");
    }
    result
}

fn decode_output(
    output: &HelperProcessOutput,
    path: &Path,
    nonce: &str,
) -> Result<Option<PreviewPayload>> {
    if !output.status.success() {
        bail!(
            "isolated embedded preview exited with {}{}",
            output.status,
            helper_stderr_suffix(&output.stderr)
        );
    }
    read_embedded_preview_output(&output.stdout, path, nonce)
}

pub(super) fn read_embedded_preview_output(
    stdout: &[u8],
    path: &Path,
    nonce: &str,
) -> Result<Option<PreviewPayload>> {
    let Some(descriptor) = parse_descriptor(stdout, nonce)? else {
        return Ok(None);
    };
    let metadata =
        fs::symlink_metadata(path).context("inspect isolated embedded preview output")?;
    if !metadata.is_file() || metadata.len() != descriptor.encoded_bytes {
        bail!("isolated embedded preview has an invalid file or byte length");
    }
    let bytes = fs::read(path).context("read bounded isolated camera JPEG")?;
    if bytes.len() as u64 != descriptor.encoded_bytes
        || bytes.len() < 4
        || bytes[..2] != [0xff, 0xd8]
        || bytes[bytes.len() - 2..] != [0xff, 0xd9]
    {
        bail!("isolated embedded preview is not the declared JPEG");
    }
    Ok(Some(PreviewPayload {
        descriptor,
        byte_order: PreviewByteOrder::NotApplicable,
        bytes,
    }))
}

fn parse_descriptor(stdout: &[u8], nonce: &str) -> Result<Option<PreviewDescriptorSnapshot>> {
    let text = std::str::from_utf8(stdout).context("decode embedded preview protocol")?;
    let fields = text.split_whitespace().collect::<Vec<_>>();
    if fields.len() < 3 || fields[0] != EMBEDDED_PREVIEW_PROTOCOL || fields[1] != nonce {
        bail!("isolated embedded preview returned an invalid protocol or nonce");
    }
    if fields.len() == 3 && fields[2] == "none" {
        return Ok(None);
    }
    if fields.len() != 9 || fields[2] != "jpeg" {
        bail!("isolated embedded preview returned an invalid layout");
    }
    let descriptor = PreviewDescriptorSnapshot {
        provider_id: fields[3].parse().context("parse camera preview id")?,
        codec: PreviewCodec::Jpeg,
        dimensions: ImageDimensions {
            width: fields[4].parse().context("parse camera preview width")?,
            height: fields[5].parse().context("parse camera preview height")?,
        },
        bits_per_channel: fields[6].parse().context("parse camera preview depth")?,
        channels: fields[7].parse().context("parse camera preview channels")?,
        encoded_bytes: fields[8]
            .parse()
            .context("parse camera preview byte length")?,
        decodable: true,
    };
    if descriptor.dimensions.width == 0
        || descriptor.dimensions.height == 0
        || descriptor
            .dimensions
            .width
            .max(descriptor.dimensions.height)
            > MAX_PREVIEW_SIDE
        || descriptor.bits_per_channel != 8
        || !matches!(descriptor.channels, 1 | 3)
        || descriptor.encoded_bytes < 4
        || descriptor.encoded_bytes > MAX_PREVIEW_BYTES
    {
        bail!("isolated embedded preview exceeded its bounded JPEG contract");
    }
    Ok(Some(descriptor))
}

#[cfg(test)]
mod tests;
