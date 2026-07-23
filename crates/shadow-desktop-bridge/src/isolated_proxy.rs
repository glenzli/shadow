//! Crash-isolated generated-preview rendering for the desktop catalog.
//!
//! Embedded camera previews never enter this path. It exists for files that
//! need a native RAW/raster reference render and may therefore cross a third
//! party decoder or a user's private provider module. A provider can return a
//! normal error, but it can also crash the process; the latter must degrade one
//! preview rather than terminate Shadow.

use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

use anyhow::{Context, Result, bail};
use shadow_domain::{ImageDimensions, PreviewCodec, ProxyPayload};
use uuid::Uuid;

const HELPER_PATH_ENVIRONMENT: &str = "SHADOW_DECODE_HELPER_PATH";
const DISABLE_PRIVATE_DECODER_ENVIRONMENT: &str = "SHADOW_DISABLE_PRIVATE_DECODER";
const HELPER_PROTOCOL: &str = "shadow-proxy-v1";
const MAX_PROXY_BYTES: u64 = 64 * 1024 * 1024;

/// Renders a bounded generated proxy in a child process.
///
/// The helper writes its JPEG into a short-lived cache-root path and sends only
/// small metadata over stdout. `Command::output` reports a crash as a failed
/// child status, allowing the scan to record a per-photo preview failure while
/// the desktop application remains alive.
pub(crate) fn render_isolated_photo_reference_proxy(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<ProxyPayload> {
    if max_edge == 0 {
        bail!("generated proxy edge must be non-zero");
    }
    if !(1..=100).contains(&jpeg_quality) {
        bail!("generated proxy JPEG quality must be in 1..=100");
    }
    let output_path = runtime_output_path(runtime_cache_root)?;
    let output = Command::new(helper_path)
        // The desktop host must not load a private native SDK. The helper is
        // its intentional isolation boundary, so it alone is allowed to probe
        // an installed private provider after the public decoder declines.
        .env_remove(DISABLE_PRIVATE_DECODER_ENVIRONMENT)
        .arg("proxy")
        .arg(source_path)
        .arg(&output_path)
        .arg(max_edge.to_string())
        .arg(jpeg_quality.to_string())
        .output()
        .with_context(|| format!("start isolated RAW decoder {}", helper_path.display()));
    let result = match output {
        Ok(output) => decode_helper_output(&output, &output_path, max_edge),
        Err(error) => Err(error),
    };
    // The child alone owns this path and it has a UUID name. Cleanup is still
    // best-effort so an interrupted helper cannot grow the cache indefinitely.
    let _ = fs::remove_file(&output_path);
    result
}

/// Returns the helper explicitly selected by the desktop shell, if present.
/// Keeping lookup outside the renderer makes tests and non-desktop clients
/// retain their direct public-provider behavior.
pub(crate) fn configured_helper_path() -> Option<PathBuf> {
    env::var_os(HELPER_PATH_ENVIRONMENT).map(PathBuf::from)
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
    output: &std::process::Output,
    output_path: &Path,
    max_edge: u32,
) -> Result<ProxyPayload> {
    if !output.status.success() {
        let detail = String::from_utf8_lossy(&output.stderr).trim().to_owned();
        let status = output.status.to_string();
        bail!(
            "isolated RAW decoder exited with {status}{}",
            if detail.is_empty() {
                String::new()
            } else {
                format!(": {detail}")
            }
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
    if fields.len() != 5 || fields[0] != HELPER_PROTOCOL || fields[3] != "8" || fields[4] != "3" {
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
mod tests {
    use super::*;

    #[test]
    fn parses_the_tiny_helper_metadata_protocol() {
        assert_eq!(
            parse_helper_dimensions(b"shadow-proxy-v1 1200 800 8 3\n").expect("parse protocol"),
            ImageDimensions {
                width: 1200,
                height: 800,
            }
        );
    }

    #[test]
    fn rejects_a_protocol_with_unexpected_pixel_layout() {
        assert!(parse_helper_dimensions(b"shadow-proxy-v1 1200 800 16 3\n").is_err());
    }
}
