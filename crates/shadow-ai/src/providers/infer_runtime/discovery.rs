//! Consumer endpoint discovery for the local Infer Runtime data plane.
//!
//! This owner validates Infra Discovery filesystem and lease facts, selects
//! one exact Consumer offer, and retains only the endpoint generation needed
//! to invalidate a failed connection. Credentials and Intent policy remain in
//! Infer Runtime's authenticated HTTP contract.

use std::{
    collections::HashSet,
    env, fs,
    io::Read,
    path::{Path, PathBuf},
    process::Command,
    sync::Mutex,
};

use reqwest::Url;
use serde::Deserialize;
use thiserror::Error;
use time::{Duration, OffsetDateTime, format_description::well_known::Rfc3339};

use super::validate_loopback_base_url;

const DISCOVERY_SCHEMA: &str = "infra.discovery.registration";
const DISCOVERY_SCHEMA_VERSION: &str = "20260810.1";
const SERVICE_KIND: &str = "infer-runtime";
const SERVICE_INSTANCE_ID: &str = "local";
const CONSUMER_PROTOCOL: &str = "infer-runtime.consumer";
const CONSUMER_PROTOCOL_VERSION_CANDIDATE_2: &str = "0.1.0-candidate.2";
const CONSUMER_PROTOCOL_VERSION_CANDIDATE_3: &str = "0.1.0-candidate.3";
const CONSUMER_BINDING: &str = "infer-runtime.http-loopback";
const FALLBACK_ENDPOINT: &str = "http://127.0.0.1:8787";
const MAX_MANIFEST_BYTES: usize = 64 * 1024;
const MAX_OFFERS: usize = 64;
const MAX_PROTOCOL_VERSIONS: usize = 16;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(super) enum InferRuntimeConsumerVersion {
    Candidate2,
    Candidate3,
}

impl InferRuntimeConsumerVersion {
    const PREFERENCE_ORDER: [Self; 2] = [Self::Candidate3, Self::Candidate2];

    pub(super) const fn as_str(self) -> &'static str {
        match self {
            Self::Candidate2 => CONSUMER_PROTOCOL_VERSION_CANDIDATE_2,
            Self::Candidate3 => CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(super) struct DiscoveryEndpoint {
    pub(super) base_url: Url,
    pub(super) consumer_version: InferRuntimeConsumerVersion,
    source: DiscoveryEndpointSource,
}

impl DiscoveryEndpoint {
    pub(super) fn explicit(base_url: Url) -> Self {
        Self {
            base_url,
            // An explicit URL has no authenticated version offer. Preserve the
            // pre-migration vocabulary instead of guessing a newer contract.
            consumer_version: InferRuntimeConsumerVersion::Candidate2,
            source: DiscoveryEndpointSource::Explicit,
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
enum DiscoveryEndpointSource {
    Explicit,
    Discovered {
        instance_id: String,
        generation: String,
        expires_at: OffsetDateTime,
    },
    CompatibilityFallback,
}

#[derive(Debug)]
pub(super) struct InferRuntimeDiscoveryResolver {
    runtime_root: Option<PathBuf>,
    fallback: Url,
    cached: Mutex<Option<DiscoveryEndpoint>>,
}

impl InferRuntimeDiscoveryResolver {
    pub(super) fn from_environment() -> Self {
        Self {
            runtime_root: runtime_root_from_environment().ok(),
            fallback: validate_loopback_base_url(FALLBACK_ENDPOINT)
                .expect("the fixed Infer Runtime fallback is canonical"),
            cached: Mutex::new(None),
        }
    }

    #[cfg(test)]
    fn from_root(runtime_root: PathBuf) -> Self {
        Self::from_root_with_fallback(runtime_root, FALLBACK_ENDPOINT)
    }

    #[cfg(test)]
    fn from_root_with_fallback(runtime_root: PathBuf, fallback: &str) -> Self {
        Self {
            runtime_root: Some(runtime_root),
            fallback: validate_loopback_base_url(fallback)
                .expect("the test Infer Runtime fallback is canonical"),
            cached: Mutex::new(None),
        }
    }

    #[cfg(test)]
    fn cached_endpoint(&self) -> Option<DiscoveryEndpoint> {
        self.cached
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
    }

    pub(super) fn resolve(&self) -> DiscoveryEndpoint {
        self.resolve_at(OffsetDateTime::now_utc())
    }

    fn resolve_at(&self, now: OffsetDateTime) -> DiscoveryEndpoint {
        let endpoint = self
            .runtime_root
            .as_deref()
            .and_then(|root| discover_endpoint(root, now).ok())
            .unwrap_or_else(|| self.fallback());
        *self
            .cached
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(endpoint.clone());
        endpoint
    }

    pub(super) fn resolve_after_connection_failure(
        &self,
        failed: &DiscoveryEndpoint,
    ) -> DiscoveryEndpoint {
        let now = OffsetDateTime::now_utc();
        let rediscovered = self
            .runtime_root
            .as_deref()
            .and_then(|root| discover_endpoint(root, now).ok());
        let endpoint = match rediscovered {
            Some(candidate) if candidate != *failed => candidate,
            Some(_) | None => self.fallback(),
        };
        *self
            .cached
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(endpoint.clone());
        endpoint
    }

    fn fallback(&self) -> DiscoveryEndpoint {
        DiscoveryEndpoint {
            base_url: self.fallback.clone(),
            // The fixed endpoint predates candidate.3 discovery. It remains a
            // candidate.2 compatibility path until the fallback is removed.
            consumer_version: InferRuntimeConsumerVersion::Candidate2,
            source: DiscoveryEndpointSource::CompatibilityFallback,
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Registration {
    schema: String,
    schema_version: String,
    service: Service,
    lease: Lease,
    offers: Vec<Offer>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Service {
    kind: String,
    instance_id: String,
    generation: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Lease {
    renewed_at: String,
    expires_at: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Offer {
    protocol: String,
    protocol_versions: Vec<String>,
    binding: String,
    endpoint: String,
}

fn discover_endpoint(
    runtime_root: &Path,
    now: OffsetDateTime,
) -> Result<DiscoveryEndpoint, InferRuntimeDiscoveryError> {
    let registrations = runtime_root.join("registrations");
    validate_private_directory(runtime_root)?;
    validate_private_directory(&registrations)?;
    #[cfg(unix)]
    validate_private_directory(&runtime_root.join("sockets"))?;
    let manifest = registrations.join(format!("{SERVICE_KIND}--{SERVICE_INSTANCE_ID}.json"));
    let registration = read_private_registration(&manifest)?;
    validate_registration(&registration, now)?;
    let (consumer_version, offer) = InferRuntimeConsumerVersion::PREFERENCE_ORDER
        .iter()
        .find_map(|version| {
            registration
                .offers
                .iter()
                .find(|offer| {
                    offer.protocol == CONSUMER_PROTOCOL
                        && offer.binding == CONSUMER_BINDING
                        && offer
                            .protocol_versions
                            .iter()
                            .any(|offered| offered == version.as_str())
                })
                .map(|offer| (*version, offer))
        })
        .ok_or(InferRuntimeDiscoveryError::NoCompatibleOffer)?;
    let base_url = validate_loopback_base_url(&offer.endpoint)
        .map_err(|_| InferRuntimeDiscoveryError::InvalidEndpoint)?;
    let expires_at = parse_time(&registration.lease.expires_at)?;
    Ok(DiscoveryEndpoint {
        base_url,
        consumer_version,
        source: DiscoveryEndpointSource::Discovered {
            instance_id: registration.service.instance_id.clone(),
            generation: registration.service.generation.clone(),
            expires_at,
        },
    })
}

fn read_private_registration(path: &Path) -> Result<Registration, InferRuntimeDiscoveryError> {
    validate_private_manifest(path)?;
    let mut file = fs::File::open(path)
        .map_err(|source| InferRuntimeDiscoveryError::Io(path.to_path_buf(), source))?;
    validate_private_manifest_metadata(
        path,
        &file
            .metadata()
            .map_err(|source| InferRuntimeDiscoveryError::Io(path.to_path_buf(), source))?,
    )?;
    let mut bytes = Vec::new();
    file.by_ref()
        .take((MAX_MANIFEST_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|source| InferRuntimeDiscoveryError::Io(path.to_path_buf(), source))?;
    if bytes.len() > MAX_MANIFEST_BYTES {
        return Err(InferRuntimeDiscoveryError::ManifestTooLarge);
    }
    serde_json::from_slice(&bytes).map_err(InferRuntimeDiscoveryError::InvalidJson)
}

fn validate_registration(
    registration: &Registration,
    now: OffsetDateTime,
) -> Result<(), InferRuntimeDiscoveryError> {
    if registration.schema != DISCOVERY_SCHEMA
        || registration.schema_version != DISCOVERY_SCHEMA_VERSION
        || registration.service.kind != SERVICE_KIND
        || registration.service.instance_id != SERVICE_INSTANCE_ID
        || !valid_file_token(&registration.service.instance_id, 96)
        || !valid_file_token(&registration.service.generation, 96)
        || registration.offers.is_empty()
        || registration.offers.len() > MAX_OFFERS
    {
        return Err(InferRuntimeDiscoveryError::InvalidRegistration);
    }
    let renewed_at = parse_time(&registration.lease.renewed_at)?;
    let expires_at = parse_time(&registration.lease.expires_at)?;
    if renewed_at >= expires_at
        || expires_at - renewed_at > Duration::seconds(120)
        || renewed_at > now + Duration::seconds(15)
        || expires_at > now + Duration::seconds(120)
        || expires_at <= now
    {
        return Err(InferRuntimeDiscoveryError::InvalidLease);
    }
    for offer in &registration.offers {
        if !valid_contract_id(&offer.protocol)
            || !valid_contract_id(&offer.binding)
            || offer.endpoint.is_empty()
            || offer.endpoint.len() > 512
            || offer.protocol_versions.is_empty()
            || offer.protocol_versions.len() > MAX_PROTOCOL_VERSIONS
            || offer
                .protocol_versions
                .iter()
                .any(|version| !valid_contract_version(version))
        {
            return Err(InferRuntimeDiscoveryError::InvalidRegistration);
        }
        let unique_versions = offer.protocol_versions.iter().collect::<HashSet<_>>();
        if unique_versions.len() != offer.protocol_versions.len()
            || (offer.binding == "infra.local.unix-socket"
                && !valid_unix_socket_endpoint(&offer.endpoint))
            || (offer.binding == "infra.local.windows-named-pipe"
                && !valid_windows_pipe_endpoint(&offer.endpoint))
        {
            return Err(InferRuntimeDiscoveryError::InvalidRegistration);
        }
    }
    Ok(())
}

fn parse_time(value: &str) -> Result<OffsetDateTime, InferRuntimeDiscoveryError> {
    if value.len() > 40 {
        return Err(InferRuntimeDiscoveryError::InvalidLease);
    }
    OffsetDateTime::parse(value, &Rfc3339).map_err(|_| InferRuntimeDiscoveryError::InvalidLease)
}

fn valid_file_token(value: &str, maximum: usize) -> bool {
    (1..=maximum).contains(&value.len())
        && value
            .as_bytes()
            .first()
            .is_some_and(u8::is_ascii_alphanumeric)
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
}

fn valid_contract_id(value: &str) -> bool {
    (1..=128).contains(&value.len())
        && value
            .as_bytes()
            .first()
            .is_some_and(u8::is_ascii_alphanumeric)
        && value.bytes().all(|byte| {
            byte.is_ascii_alphanumeric()
                || matches!(byte, b'.' | b'_' | b':' | b'+' | b'/' | b'@' | b'%' | b'-')
        })
}

fn valid_contract_version(value: &str) -> bool {
    (1..=64).contains(&value.len())
        && value
            .as_bytes()
            .first()
            .is_some_and(u8::is_ascii_alphanumeric)
        && value.bytes().all(|byte| {
            byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b':' | b'+' | b'-')
        })
}

fn valid_unix_socket_endpoint(value: &str) -> bool {
    let Some(opaque) = value
        .strip_prefix("sockets/")
        .and_then(|name| name.strip_suffix(".sock"))
    else {
        return false;
    };
    valid_file_token(opaque, 16)
}

fn valid_windows_pipe_endpoint(value: &str) -> bool {
    value
        .strip_prefix(r"\\.\pipe\infra-protocol\")
        .is_some_and(|opaque| valid_file_token(opaque, 64))
}

fn runtime_root_from_environment() -> Result<PathBuf, InferRuntimeDiscoveryError> {
    if let Some(override_root) = env::var_os("INFRA_PROTOCOL_RUNTIME_DIR") {
        let root = PathBuf::from(override_root);
        return root
            .is_absolute()
            .then_some(root)
            .ok_or(InferRuntimeDiscoveryError::RuntimeRootUnavailable);
    }
    platform_runtime_root()
}

#[cfg(target_os = "macos")]
fn platform_runtime_root() -> Result<PathBuf, InferRuntimeDiscoveryError> {
    let output = Command::new("/usr/bin/getconf")
        .arg("DARWIN_USER_TEMP_DIR")
        .output()
        .map_err(|_| InferRuntimeDiscoveryError::RuntimeRootUnavailable)?;
    if !output.status.success() {
        return Err(InferRuntimeDiscoveryError::RuntimeRootUnavailable);
    }
    let base = String::from_utf8(output.stdout)
        .map_err(|_| InferRuntimeDiscoveryError::RuntimeRootUnavailable)?;
    let base = PathBuf::from(base.trim());
    if !base.is_absolute() {
        return Err(InferRuntimeDiscoveryError::RuntimeRootUnavailable);
    }
    Ok(base.join("infra-protocol"))
}

#[cfg(target_os = "linux")]
fn platform_runtime_root() -> Result<PathBuf, InferRuntimeDiscoveryError> {
    let base = env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .ok_or(InferRuntimeDiscoveryError::RuntimeRootUnavailable)?;
    Ok(base.join("infra-protocol"))
}

#[cfg(windows)]
fn platform_runtime_root() -> Result<PathBuf, InferRuntimeDiscoveryError> {
    let output = powershell()
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "[Environment]::GetFolderPath('LocalApplicationData')",
        ])
        .output()
        .map_err(|_| InferRuntimeDiscoveryError::RuntimeRootUnavailable)?;
    if !output.status.success() {
        return Err(InferRuntimeDiscoveryError::RuntimeRootUnavailable);
    }
    let base = String::from_utf8(output.stdout)
        .map_err(|_| InferRuntimeDiscoveryError::RuntimeRootUnavailable)?;
    let base = PathBuf::from(base.trim());
    if !base.is_absolute() {
        return Err(InferRuntimeDiscoveryError::RuntimeRootUnavailable);
    }
    Ok(base.join("Infra Protocol").join("Runtime"))
}

#[cfg(not(any(target_os = "macos", target_os = "linux", windows)))]
fn platform_runtime_root() -> Result<PathBuf, InferRuntimeDiscoveryError> {
    Err(InferRuntimeDiscoveryError::RuntimeRootUnavailable)
}

#[cfg(unix)]
fn validate_private_directory(path: &Path) -> Result<(), InferRuntimeDiscoveryError> {
    use std::os::unix::fs::{MetadataExt, PermissionsExt};

    let metadata = fs::symlink_metadata(path)
        .map_err(|source| InferRuntimeDiscoveryError::Io(path.to_path_buf(), source))?;
    if metadata.file_type().is_symlink()
        || !metadata.is_dir()
        || metadata.uid() != effective_uid()?
        || metadata.permissions().mode() & 0o777 != 0o700
    {
        return Err(InferRuntimeDiscoveryError::UnsafeObject(path.to_path_buf()));
    }
    Ok(())
}

#[cfg(unix)]
fn validate_private_manifest(path: &Path) -> Result<(), InferRuntimeDiscoveryError> {
    let metadata = fs::symlink_metadata(path)
        .map_err(|source| InferRuntimeDiscoveryError::Io(path.to_path_buf(), source))?;
    if metadata.file_type().is_symlink() {
        return Err(InferRuntimeDiscoveryError::UnsafeObject(path.to_path_buf()));
    }
    validate_private_manifest_metadata(path, &metadata)
}

#[cfg(unix)]
fn validate_private_manifest_metadata(
    path: &Path,
    metadata: &fs::Metadata,
) -> Result<(), InferRuntimeDiscoveryError> {
    use std::os::unix::fs::{MetadataExt, PermissionsExt};

    if !metadata.is_file()
        || metadata.uid() != effective_uid()?
        || metadata.permissions().mode() & 0o777 != 0o600
    {
        return Err(InferRuntimeDiscoveryError::UnsafeObject(path.to_path_buf()));
    }
    if metadata.len() > MAX_MANIFEST_BYTES as u64 {
        return Err(InferRuntimeDiscoveryError::ManifestTooLarge);
    }
    Ok(())
}

#[cfg(unix)]
fn effective_uid() -> Result<u32, InferRuntimeDiscoveryError> {
    let output = Command::new("/usr/bin/id")
        .arg("-u")
        .output()
        .map_err(|_| InferRuntimeDiscoveryError::CurrentUserUnavailable)?;
    if !output.status.success() {
        return Err(InferRuntimeDiscoveryError::CurrentUserUnavailable);
    }
    String::from_utf8(output.stdout)
        .ok()
        .and_then(|value| value.trim().parse::<u32>().ok())
        .ok_or(InferRuntimeDiscoveryError::CurrentUserUnavailable)
}

#[cfg(windows)]
fn validate_private_directory(path: &Path) -> Result<(), InferRuntimeDiscoveryError> {
    validate_windows_private_object(path, true)
}

#[cfg(windows)]
fn validate_private_manifest(path: &Path) -> Result<(), InferRuntimeDiscoveryError> {
    validate_windows_private_object(path, false)?;
    let metadata = fs::metadata(path)
        .map_err(|source| InferRuntimeDiscoveryError::Io(path.to_path_buf(), source))?;
    validate_private_manifest_metadata(path, &metadata)
}

#[cfg(windows)]
fn validate_private_manifest_metadata(
    path: &Path,
    metadata: &fs::Metadata,
) -> Result<(), InferRuntimeDiscoveryError> {
    if !metadata.is_file() || metadata.len() > MAX_MANIFEST_BYTES as u64 {
        return Err(InferRuntimeDiscoveryError::UnsafeObject(path.to_path_buf()));
    }
    Ok(())
}

#[cfg(windows)]
fn validate_windows_private_object(
    path: &Path,
    expect_directory: bool,
) -> Result<(), InferRuntimeDiscoveryError> {
    const VALIDATE_ACL: &str = r#"
$ErrorActionPreference = 'Stop'
$item = Get-Item -LiteralPath $env:SHADOW_INFRA_DISCOVERY_PATH -Force
if ([bool]($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { exit 10 }
if ($item.PSIsContainer -ne ($env:SHADOW_INFRA_DISCOVERY_DIRECTORY -eq '1')) { exit 11 }
$current = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$system = 'S-1-5-18'
$administrators = 'S-1-5-32-544'
$acl = Get-Acl -LiteralPath $item.FullName
$owner = $acl.Owner
try { $owner = (New-Object Security.Principal.NTAccount($owner)).Translate([Security.Principal.SecurityIdentifier]).Value } catch {}
if ($owner -ne $current) { exit 12 }
foreach ($rule in $acl.Access) {
  if ($rule.AccessControlType -ne [Security.AccessControl.AccessControlType]::Allow) { continue }
  $sid = $rule.IdentityReference
  try { $sid = $sid.Translate([Security.Principal.SecurityIdentifier]).Value } catch { exit 13 }
  if ($sid -ne $current -and $sid -ne $system -and $sid -ne $administrators) { exit 14 }
}
"#;
    let status = powershell()
        .args(["-NoProfile", "-NonInteractive", "-Command", VALIDATE_ACL])
        .env("SHADOW_INFRA_DISCOVERY_PATH", path)
        .env(
            "SHADOW_INFRA_DISCOVERY_DIRECTORY",
            if expect_directory { "1" } else { "0" },
        )
        .status()
        .map_err(|_| InferRuntimeDiscoveryError::UnsafeObject(path.to_path_buf()))?;
    if !status.success() {
        return Err(InferRuntimeDiscoveryError::UnsafeObject(path.to_path_buf()));
    }
    Ok(())
}

#[cfg(windows)]
fn powershell() -> Command {
    let executable = env::var_os("SystemRoot")
        .map(PathBuf::from)
        .map(|root| {
            root.join("System32")
                .join("WindowsPowerShell")
                .join("v1.0")
                .join("powershell.exe")
        })
        .unwrap_or_else(|| PathBuf::from("powershell.exe"));
    Command::new(executable)
}

#[derive(Debug, Error)]
enum InferRuntimeDiscoveryError {
    #[error("Infra Discovery runtime root is unavailable")]
    RuntimeRootUnavailable,
    #[error("cannot determine the current local user")]
    CurrentUserUnavailable,
    #[error("Infra Discovery object is not owner-only: {0}")]
    UnsafeObject(PathBuf),
    #[error("Infra Discovery manifest exceeds 64 KiB")]
    ManifestTooLarge,
    #[error("Infra Discovery I/O failed for {0}: {1}")]
    Io(PathBuf, #[source] std::io::Error),
    #[error("Infra Discovery manifest is not strict contract JSON: {0}")]
    InvalidJson(serde_json::Error),
    #[error("Infra Discovery registration shape is invalid")]
    InvalidRegistration,
    #[error("Infra Discovery lease is invalid or expired")]
    InvalidLease,
    #[error("Infer Runtime has no compatible Consumer offer")]
    NoCompatibleOffer,
    #[error("Infer Runtime Consumer endpoint is invalid")]
    InvalidEndpoint,
}

#[cfg(test)]
mod tests;
