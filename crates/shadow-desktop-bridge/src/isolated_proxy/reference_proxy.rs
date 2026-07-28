//! Crash-isolated bounded JPEG reference-proxy rendering.

use std::{
    fs,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use shadow_domain::{ImageDimensions, PreviewCodec, ProxyPayload};
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
    route_identity::{DecoderSafetyKey, REFERENCE_PROXY_PROTOCOL},
};

const MAX_PROXY_BYTES: u64 = 64 * 1024 * 1024;

/// Renders a bounded generated proxy in a child process.
///
/// The helper writes its JPEG into a short-lived cache-root path and sends only
/// small metadata over stdout. A bounded child wait converts a crash or hung
/// decoder into a per-photo preview failure while the desktop application
/// remains alive.
pub(crate) fn render_isolated_photo_reference_proxy(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<ProxyPayload> {
    let (proxy, output_path) = render_isolated_photo_reference_proxy_to_file(
        helper_path,
        runtime_cache_root,
        source_path,
        max_edge,
        jpeg_quality,
    )?;
    // The child alone owns this path and it has a UUID name. Cleanup is still
    // best-effort so an interrupted helper cannot grow the cache indefinitely.
    let _ = fs::remove_file(output_path);
    Ok(proxy)
}

/// Renders a bounded proxy and leaves its JPEG available to the caller briefly.
///
/// This is used only to bridge a private RAW provider into the ordinary, public
/// raster edit pipeline. The caller must remove `PathBuf` after opening it: the
/// C++ edit sessions copy their prepared pixels and retain no source file handle.
pub(crate) fn render_isolated_photo_reference_proxy_to_file(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<(ProxyPayload, PathBuf)> {
    if max_edge == 0 {
        bail!("generated proxy edge must be non-zero");
    }
    if !(1..=100).contains(&jpeg_quality) {
        bail!("generated proxy JPEG quality must be in 1..=100");
    }
    let safety_key = DecoderSafetyKey::reference_proxy(source_path, helper_path)?;
    if let Some(observation) =
        DecoderSafetyRegistry::shared().observation(runtime_cache_root, &safety_key)?
        && observation.quarantines_main_process_native_decode()
    {
        bail!(
            "isolated RAW decoder is quarantined for this unchanged source after {}; change the source or helper before retrying native decode",
            observation.diagnostic_label()
        );
    }
    let output_path = runtime_output_path(runtime_cache_root)?;
    let execution = execute_decode_helper(helper_path, |command| {
        command
            .arg("proxy")
            .arg(source_path)
            .arg(&output_path)
            .arg(max_edge.to_string())
            .arg(jpeg_quality.to_string());
    });
    let (result, observation) = match execution {
        Ok(HelperExecution::TimedOut(output)) => (
            Err(anyhow::anyhow!(
                "isolated RAW decoder timed out after {} seconds{}",
                HELPER_TIMEOUT.as_secs(),
                helper_stderr_suffix(&output.stderr)
            )),
            IsolatedDecodeObservation::TimedOut,
        ),
        Ok(HelperExecution::Completed(output)) => {
            let result = decode_helper_output(&output, &output_path, max_edge);
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
    record_safety_observation_if_source_is_current(
        runtime_cache_root,
        source_path,
        helper_path,
        &safety_key,
        observation,
    );
    match result {
        Ok(proxy) => Ok((proxy, output_path)),
        Err(error) => {
            let _ = fs::remove_file(&output_path);
            Err(error)
        }
    }
}

fn runtime_output_path(runtime_cache_root: &Path) -> Result<PathBuf> {
    let directory = runtime_cache_root.join("decode-helper");
    fs::create_dir_all(&directory).with_context(|| {
        format!(
            "create isolated decode cache directory {}",
            directory.display()
        )
    })?;
    Ok(directory.join(format!("{}.jpg", Uuid::now_v7())))
}

fn decode_helper_output(
    output: &HelperProcessOutput,
    output_path: &Path,
    max_edge: u32,
) -> Result<ProxyPayload> {
    if !output.status.success() {
        let status = output.status.to_string();
        bail!(
            "isolated RAW decoder exited with {status}{}",
            helper_stderr_suffix(&output.stderr)
        );
    }
    let dimensions = parse_helper_dimensions(&output.stdout)?;
    if dimensions.width == 0 || dimensions.height == 0 {
        bail!("isolated RAW decoder returned zero-sized proxy dimensions");
    }
    if dimensions.width.max(dimensions.height) > max_edge {
        bail!("isolated RAW decoder exceeded requested proxy edge");
    }
    let metadata = fs::metadata(output_path)
        .with_context(|| format!("read isolated proxy output {}", output_path.display()))?;
    if metadata.len() == 0 || metadata.len() > MAX_PROXY_BYTES {
        bail!("isolated RAW decoder produced an invalid proxy byte length");
    }
    let bytes = fs::read(output_path)
        .with_context(|| format!("read isolated proxy output {}", output_path.display()))?;
    if bytes.len() < 4 || bytes[..2] != [0xff, 0xd8] || bytes[bytes.len() - 2..] != [0xff, 0xd9] {
        bail!("isolated RAW decoder produced a non-JPEG proxy");
    }
    Ok(ProxyPayload {
        dimensions,
        codec: PreviewCodec::Jpeg,
        bits_per_channel: 8,
        channels: 3,
        bytes,
    })
}

fn parse_helper_dimensions(stdout: &[u8]) -> Result<ImageDimensions> {
    let stdout = std::str::from_utf8(stdout).context("decode isolated RAW helper output")?;
    let fields = stdout.split_whitespace().collect::<Vec<_>>();
    if fields.len() != 5
        || fields[0] != REFERENCE_PROXY_PROTOCOL
        || fields[3] != "8"
        || fields[4] != "3"
    {
        bail!("isolated RAW decoder returned an invalid protocol response");
    }
    let width = fields[1]
        .parse::<u32>()
        .context("parse isolated RAW proxy width")?;
    let height = fields[2]
        .parse::<u32>()
        .context("parse isolated RAW proxy height")?;
    Ok(ImageDimensions { width, height })
}

#[cfg(test)]
mod tests;
