//! Short-lived provider-neutral `RawFrame` staging for local AI sidecars.
//!
//! The private decoder stays in the existing crash-isolated helper. Only an
//! active little-endian Bayer plane and a bounded technical manifest enter a
//! request-private cache directory; RAII cleanup removes both after planning
//! or materialization.

use std::{
    fs,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use uuid::Uuid;

use super::helper_process::{
    HELPER_TIMEOUT, HelperExecution, HelperProcessOutput, execute_decode_helper,
    helper_stderr_suffix,
};

const PROTOCOL: &str = "shadow-raw-frame-staging-20260806.1";
const MAX_MANIFEST_BYTES: u64 = 16 * 1024;
const MAX_SAMPLE_BYTES: u64 = 512 * 1024 * 1024;

#[derive(Debug)]
pub(crate) struct IsolatedRawFrameStaging {
    directory: PathBuf,
    manifest_path: PathBuf,
}

impl IsolatedRawFrameStaging {
    pub(crate) fn manifest_path(&self) -> &Path {
        &self.manifest_path
    }
}

impl Drop for IsolatedRawFrameStaging {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.directory);
    }
}

pub(crate) fn stage_isolated_raw_frame(
    helper_path: &Path,
    staging_root: &Path,
    source_path: &Path,
) -> Result<IsolatedRawFrameStaging> {
    fs::create_dir_all(staging_root).with_context(|| {
        format!(
            "create isolated RAW frame staging root {}",
            staging_root.display()
        )
    })?;
    let nonce = Uuid::now_v7();
    let directory = staging_root.join(nonce.to_string());
    fs::create_dir(&directory).with_context(|| {
        format!(
            "create isolated RAW frame staging transaction {}",
            directory.display()
        )
    })?;
    let staging = IsolatedRawFrameStaging {
        manifest_path: directory.join("frame.shadowrawi"),
        directory,
    };
    let execution = execute_decode_helper(helper_path, |command| {
        command
            .arg("raw-frame-staging")
            .arg(source_path)
            .arg(&staging.manifest_path)
            .arg(nonce.to_string());
    })?;
    match execution {
        HelperExecution::TimedOut(output) => bail!(
            "isolated RAW decoder timed out after {} seconds{}",
            HELPER_TIMEOUT.as_secs(),
            helper_stderr_suffix(&output.stderr)
        ),
        HelperExecution::Completed(output) => {
            validate_helper_output(&output, &staging.manifest_path, nonce)?;
        }
    }
    Ok(staging)
}

fn validate_helper_output(
    output: &HelperProcessOutput,
    manifest_path: &Path,
    nonce: Uuid,
) -> Result<()> {
    if !output.status.success() {
        bail!(
            "isolated RAW decoder rejected AI Bayer staging{}",
            helper_stderr_suffix(&output.stderr)
        );
    }
    let stdout =
        std::str::from_utf8(&output.stdout).context("decode isolated RAW frame staging receipt")?;
    let fields = stdout.split_whitespace().collect::<Vec<_>>();
    if fields.len() != 6
        || fields[0] != PROTOCOL
        || fields[1] != "raw-frame-staging"
        || fields[2] != nonce.to_string()
    {
        bail!("isolated RAW decoder returned an invalid staging receipt");
    }
    let width = fields[3]
        .parse::<u32>()
        .context("parse staged RAW frame width")?;
    let height = fields[4]
        .parse::<u32>()
        .context("parse staged RAW frame height")?;
    let sample_bytes = fields[5]
        .parse::<u64>()
        .context("parse staged RAW frame sample bytes")?;
    let expected_bytes = u64::from(width)
        .checked_mul(u64::from(height))
        .and_then(|value| value.checked_mul(2))
        .ok_or_else(|| anyhow::anyhow!("staged RAW frame dimensions overflow"))?;
    if width == 0
        || height == 0
        || sample_bytes != expected_bytes
        || sample_bytes > MAX_SAMPLE_BYTES
    {
        bail!("isolated RAW decoder returned invalid staging dimensions");
    }
    let manifest_metadata =
        fs::metadata(manifest_path).context("read isolated RAW frame staging manifest")?;
    if !manifest_metadata.is_file()
        || manifest_metadata.len() == 0
        || manifest_metadata.len() > MAX_MANIFEST_BYTES
    {
        bail!("isolated RAW decoder returned an invalid staging manifest");
    }
    let mut sample_name = manifest_path.as_os_str().to_os_string();
    sample_name.push(".u16le");
    let sample_path = PathBuf::from(sample_name);
    let sample_metadata =
        fs::metadata(sample_path).context("read isolated RAW frame staging samples")?;
    if !sample_metadata.is_file() || sample_metadata.len() != sample_bytes {
        bail!("isolated RAW decoder returned incomplete staging samples");
    }
    Ok(())
}
