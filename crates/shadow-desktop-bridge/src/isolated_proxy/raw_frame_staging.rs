//! Short-lived provider-neutral `RawFrame` staging for local AI sidecars.
//!
//! The private decoder stays in the existing crash-isolated helper. Only an
//! active little-endian Bayer plane and a bounded technical manifest enter a
//! request-private cache directory; RAII cleanup removes both after planning
//! or materialization.

use std::{
    collections::BTreeMap,
    fs::{self, File, OpenOptions},
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use shadow_cache::sha256_file;
use uuid::Uuid;

use super::helper_process::{
    HELPER_TIMEOUT, HelperExecution, HelperProcessOutput, execute_decode_helper,
    helper_stderr_suffix,
};

const PROTOCOL: &str = "shadow-raw-frame-staging-20260822.1";
const DESCRIPTOR_CONTRACT: &str = "active-camera-colour-response-20260822.1";
const MAX_MANIFEST_BYTES: u64 = 16 * 1024;
const MAX_SAMPLE_BYTES: u64 = 512 * 1024 * 1024;
const MAX_DIMENSION: u32 = 100_000;
const EXPECTED_FIELDS: [&str; 18] = [
    "as_shot_neutral",
    "bits_per_sample",
    "black",
    "camera_to_linear_srgb_d65",
    "camera_to_xyz_d50",
    "cfa",
    "descriptor_contract",
    "height",
    "has_linear_response",
    "linear_response",
    "orientation",
    "pending_dng_opcode_bytes",
    "provider_id_hex",
    "provider_version_hex",
    "sample_bytes",
    "white",
    "width",
    "xyz_to_camera_d65",
];

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct IsolatedRawFrameDescriptor {
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) cfa: String,
    pub(crate) black_levels: [u16; 4],
    pub(crate) white_levels: [u16; 4],
    pub(crate) sample_bytes: u64,
    pub(crate) decoded_samples_sha256: String,
    pub(crate) decoder_provider_id: String,
    pub(crate) decoder_provider_version: String,
}

#[derive(Debug)]
pub(crate) struct IsolatedRawFrameStaging {
    directory: PathBuf,
    manifest_path: PathBuf,
    sample: File,
    descriptor: IsolatedRawFrameDescriptor,
}

struct StagingDirectoryGuard {
    path: PathBuf,
    armed: bool,
}

impl StagingDirectoryGuard {
    fn new(path: PathBuf) -> Self {
        Self { path, armed: true }
    }

    fn disarm(&mut self) {
        self.armed = false;
    }
}

impl Drop for StagingDirectoryGuard {
    fn drop(&mut self) {
        if self.armed {
            let _ = fs::remove_dir_all(&self.path);
        }
    }
}

impl IsolatedRawFrameStaging {
    pub(crate) fn manifest_path(&self) -> &Path {
        &self.manifest_path
    }

    pub(crate) fn try_clone_sample(&self) -> std::io::Result<File> {
        self.sample.try_clone()
    }

    pub(crate) const fn descriptor(&self) -> &IsolatedRawFrameDescriptor {
        &self.descriptor
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
    let mut directory_guard = StagingDirectoryGuard::new(directory.clone());
    harden_directory(&directory)?;
    let manifest_path = directory.join("frame.shadowrawi");
    let execution = execute_decode_helper(helper_path, |command| {
        command
            .arg("raw-frame-staging")
            .arg(source_path)
            .arg(&manifest_path)
            .arg(nonce.to_string());
    })?;
    match execution {
        HelperExecution::TimedOut(output) => bail!(
            "isolated RAW decoder timed out after {} seconds{}",
            HELPER_TIMEOUT.as_secs(),
            helper_stderr_suffix(&output.stderr)
        ),
        HelperExecution::Completed(output) => {
            validate_helper_output(&output, &manifest_path, nonce)?;
        }
    }
    let (_, sample, descriptor) = open_staged_frame(&manifest_path)?;
    directory_guard.disarm();
    Ok(IsolatedRawFrameStaging {
        directory,
        manifest_path,
        sample,
        descriptor,
    })
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
        fs::symlink_metadata(manifest_path).context("read isolated RAW frame staging manifest")?;
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
        fs::symlink_metadata(sample_path).context("read isolated RAW frame staging samples")?;
    if !sample_metadata.is_file() || sample_metadata.len() != sample_bytes {
        bail!("isolated RAW decoder returned incomplete staging samples");
    }
    Ok(())
}

fn open_staged_frame(manifest_path: &Path) -> Result<(PathBuf, File, IsolatedRawFrameDescriptor)> {
    harden_file(manifest_path)?;
    let manifest =
        fs::read_to_string(manifest_path).context("read isolated RAW frame staging manifest")?;
    if !manifest.ends_with('\n') || manifest[..manifest.len() - 1].contains('\n') {
        bail!("isolated RAW frame staging manifest is not one complete line");
    }
    let fields = parse_manifest_fields(&manifest)?;
    let width = parse_field::<u32>(&fields, "width")?;
    let height = parse_field::<u32>(&fields, "height")?;
    if width < 4 || height < 4 || width > MAX_DIMENSION || height > MAX_DIMENSION {
        bail!("isolated RAW frame staging dimensions are outside the supported bound");
    }
    let cfa = required_field(&fields, "cfa")?.to_owned();
    let mut sites = cfa.as_bytes().to_vec();
    sites.sort_unstable();
    if sites != b"BGGR" {
        bail!("isolated RAW frame staging CFA is unsupported");
    }
    let black_levels = parse_levels(required_field(&fields, "black")?, "black")?;
    let white_levels = parse_levels(required_field(&fields, "white")?, "white")?;
    if black_levels
        .iter()
        .zip(white_levels)
        .any(|(black, white)| *black >= white)
        || white_levels.iter().any(|white| *white != white_levels[0])
    {
        bail!("isolated RAW frame staging sensor levels are unsupported");
    }
    let linear_response = parse_levels(
        required_field(&fields, "linear_response")?,
        "linear-response",
    )?;
    if parse_bool01(
        required_field(&fields, "has_linear_response")?,
        "linear-response availability",
    )? && black_levels
        .iter()
        .zip(linear_response)
        .zip(white_levels)
        .any(|((black, response), white)| response <= *black || response > white)
    {
        bail!("isolated RAW frame staging linear-response limits are unsupported");
    }
    let sample_bytes = parse_field::<u64>(&fields, "sample_bytes")?;
    let expected_bytes = u64::from(width)
        .checked_mul(u64::from(height))
        .and_then(|pixels| pixels.checked_mul(2))
        .ok_or_else(|| anyhow::anyhow!("isolated RAW frame staging dimensions overflow"))?;
    if sample_bytes != expected_bytes || sample_bytes > MAX_SAMPLE_BYTES {
        bail!("isolated RAW frame staging sample byte count is invalid");
    }
    let provider_id = decode_identity(required_field(&fields, "provider_id_hex")?)?;
    let provider_version = decode_identity(required_field(&fields, "provider_version_hex")?)?;
    if provider_id.is_empty() != provider_version.is_empty() {
        bail!("isolated RAW frame staging decoder identity is incomplete");
    }
    let decoder_provider_id = if provider_id.is_empty() {
        "shadow.raw-frame".to_owned()
    } else {
        provider_id
    };
    let decoder_provider_version = if provider_version.is_empty() {
        PROTOCOL.to_owned()
    } else {
        provider_version
    };

    let mut sample_name = manifest_path.as_os_str().to_os_string();
    sample_name.push(".u16le");
    let sample_path = PathBuf::from(sample_name);
    harden_file(&sample_path)?;
    let decoded_samples_sha256 =
        sha256_file(&sample_path).context("hash isolated RAW frame staging samples")?;
    let sample = OpenOptions::new()
        .read(true)
        .open(&sample_path)
        .context("open isolated RAW frame staging samples")?;
    let metadata = sample
        .metadata()
        .context("validate opened isolated RAW frame staging samples")?;
    if !metadata.is_file() || metadata.len() != sample_bytes {
        bail!("opened isolated RAW frame staging samples changed");
    }

    Ok((
        sample_path,
        sample,
        IsolatedRawFrameDescriptor {
            width,
            height,
            cfa,
            black_levels,
            white_levels,
            sample_bytes,
            decoded_samples_sha256,
            decoder_provider_id,
            decoder_provider_version,
        },
    ))
}

fn parse_manifest_fields(manifest: &str) -> Result<BTreeMap<&str, &str>> {
    let mut tokens = manifest.split_whitespace();
    if tokens.next() != Some(PROTOCOL) {
        bail!("isolated RAW frame staging schema is unsupported");
    }
    let mut fields = BTreeMap::new();
    for token in tokens {
        let Some((key, value)) = token.split_once('=') else {
            bail!("isolated RAW frame staging manifest contains a malformed field");
        };
        if key.is_empty() || value.is_empty() || fields.insert(key, value).is_some() {
            bail!("isolated RAW frame staging manifest contains duplicate or empty fields");
        }
    }
    if fields.len() != EXPECTED_FIELDS.len()
        || !EXPECTED_FIELDS
            .iter()
            .all(|field| fields.contains_key(field))
        || required_field(&fields, "descriptor_contract")? != DESCRIPTOR_CONTRACT
    {
        bail!("isolated RAW frame staging manifest contract changed");
    }
    Ok(fields)
}

fn required_field<'fields>(
    fields: &'fields BTreeMap<&str, &str>,
    key: &str,
) -> Result<&'fields str> {
    fields
        .get(key)
        .copied()
        .ok_or_else(|| anyhow::anyhow!("isolated RAW frame staging field {key} is missing"))
}

fn parse_field<T: std::str::FromStr>(fields: &BTreeMap<&str, &str>, key: &str) -> Result<T> {
    required_field(fields, key)?
        .parse()
        .map_err(|_| anyhow::anyhow!("isolated RAW frame staging field {key} is invalid"))
}

fn parse_levels(value: &str, label: &str) -> Result<[u16; 4]> {
    value
        .split(',')
        .map(str::parse::<u16>)
        .collect::<std::result::Result<Vec<_>, _>>()
        .ok()
        .and_then(|values| values.try_into().ok())
        .ok_or_else(|| anyhow::anyhow!("isolated RAW frame staging {label} levels are invalid"))
}

fn parse_bool01(value: &str, label: &str) -> Result<bool> {
    match value {
        "0" => Ok(false),
        "1" => Ok(true),
        _ => bail!("isolated RAW frame staging {label} is invalid"),
    }
}

fn decode_identity(value: &str) -> Result<String> {
    if value == "-" {
        return Ok(String::new());
    }
    if !value.len().is_multiple_of(2)
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        bail!("isolated RAW frame staging decoder identity is not lowercase hex");
    }
    let bytes = value
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let high = hex_nibble(pair[0]);
            let low = hex_nibble(pair[1]);
            (high << 4) | low
        })
        .collect::<Vec<_>>();
    let identity = String::from_utf8(bytes)
        .context("isolated RAW frame staging decoder identity is not UTF-8")?;
    if identity.is_empty() || identity.chars().any(char::is_control) {
        bail!("isolated RAW frame staging decoder identity is invalid");
    }
    Ok(identity)
}

fn hex_nibble(byte: u8) -> u8 {
    match byte {
        b'0'..=b'9' => byte - b'0',
        b'a'..=b'f' => byte - b'a' + 10,
        _ => unreachable!("decode_identity validates lowercase hex"),
    }
}

#[cfg(unix)]
fn harden_directory(path: &Path) -> Result<()> {
    use std::os::unix::fs::PermissionsExt as _;

    fs::set_permissions(path, fs::Permissions::from_mode(0o700)).with_context(|| {
        format!(
            "make isolated RAW frame staging directory owner-only {}",
            path.display()
        )
    })
}

#[cfg(not(unix))]
fn harden_directory(_path: &Path) -> Result<()> {
    Ok(())
}

#[cfg(unix)]
fn harden_file(path: &Path) -> Result<()> {
    use std::os::unix::fs::{MetadataExt as _, PermissionsExt as _};

    let metadata = fs::symlink_metadata(path)
        .with_context(|| format!("inspect isolated RAW frame staging file {}", path.display()))?;
    if !metadata.file_type().is_file() || metadata.nlink() != 1 {
        bail!("isolated RAW frame staging file is not a singly linked regular file");
    }
    fs::set_permissions(path, fs::Permissions::from_mode(0o600)).with_context(|| {
        format!(
            "make isolated RAW frame staging file owner-only {}",
            path.display()
        )
    })
}

#[cfg(not(unix))]
fn harden_file(path: &Path) -> Result<()> {
    let metadata = fs::symlink_metadata(path)
        .with_context(|| format!("inspect isolated RAW frame staging file {}", path.display()))?;
    if !metadata.file_type().is_file() {
        bail!("isolated RAW frame staging file is not regular");
    }
    Ok(())
}

#[cfg(test)]
mod tests;
