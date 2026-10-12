use std::{
    collections::BTreeSet,
    fs,
    io::Read,
    path::{Component, Path, PathBuf},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use shadow_domain::AssetLocation;
use shadow_native_path::{native_location, native_path_from_location};

use super::{ProfileArchiveControl, transfer};

pub(super) const MANIFEST: &str = "shadow-library-backup.json";
const MAX_MANIFEST_BYTES: u64 = 128 * 1024 * 1024;

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct ArchiveManifest {
    pub schema_version: u32,
    pub source_root: AssetLocation,
    pub profile: Value,
    pub byte_count: u64,
    pub files: Vec<ArchiveFile>,
    pub owned_locations: Vec<OwnedLocation>,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct OwnedLocation {
    pub table: String,
    pub id: Vec<u8>,
    pub relative: AssetLocation,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct ArchiveFile {
    pub path: AssetLocation,
    pub byte_count: u64,
    pub sha256: String,
    pub sqlite: bool,
    pub modified: ArchiveTimestamp,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(super) struct ArchiveTimestamp {
    pub before_epoch: bool,
    pub seconds: u64,
    pub nanoseconds: u32,
}

impl ArchiveTimestamp {
    pub(super) fn from_time(time: SystemTime) -> Self {
        let (before_epoch, duration) = match time.duration_since(UNIX_EPOCH) {
            Ok(duration) => (false, duration),
            Err(error) => (true, error.duration()),
        };
        Self {
            before_epoch,
            seconds: duration.as_secs(),
            nanoseconds: duration.subsec_nanos(),
        }
    }
    pub(super) fn time(&self) -> Result<SystemTime> {
        if self.nanoseconds >= 1_000_000_000 {
            bail!("backup contains an invalid file timestamp");
        }
        let duration = Duration::new(self.seconds, self.nanoseconds);
        (if self.before_epoch {
            UNIX_EPOCH.checked_sub(duration)
        } else {
            UNIX_EPOCH.checked_add(duration)
        })
        .context("backup file timestamp exceeds the host range")
    }
}

pub(super) fn relative_path(location: &AssetLocation) -> Result<PathBuf> {
    let path = native_path_from_location(location)?;
    if path.as_os_str().is_empty()
        || path.is_absolute()
        || path
            .components()
            .any(|part| !matches!(part, Component::Normal(_)))
        || path.components().count() > 128
    {
        bail!("backup contains an invalid relative path");
    }
    Ok(path)
}

pub(super) fn profile(root: &Path, expected_id: &str) -> Result<Value> {
    let marker = root.join("storage-profile.json");
    transfer::regular_file(&marker)?;
    let mut bytes = Vec::new();
    fs::File::open(marker)?
        .take(16 * 1024 * 1024 + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() > 16 * 1024 * 1024 {
        bail!("library marker exceeds its size limit");
    }
    let value: Value = serde_json::from_slice(&bytes)?;
    let id = value["id"].as_str().context("library identity missing")?;
    uuid::Uuid::parse_str(id).context("invalid library identity")?;
    if value["schemaVersion"] != 1
        || !matches!(value["kind"].as_str(), Some("daily" | "debug" | "custom"))
        || (!expected_id.is_empty() && id != expected_id)
    {
        bail!("library identity or format changed");
    }
    Ok(value)
}

pub(super) fn verify(root: &Path, control: &ProfileArchiveControl) -> Result<ArchiveManifest> {
    let root = root.canonicalize().context("open backup directory")?;
    let marker = root.join(MANIFEST);
    transfer::regular_file(&marker)?;
    let mut bytes = Vec::new();
    fs::File::open(marker)?
        .take(MAX_MANIFEST_BYTES + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_MANIFEST_BYTES {
        bail!("backup manifest exceeds 128 MiB");
    }
    let manifest: ArchiveManifest =
        serde_json::from_slice(&bytes).context("read backup manifest")?;
    if manifest.schema_version != 1 || manifest.files.is_empty() || manifest.files.len() > 1_000_000
    {
        bail!("unsupported or incomplete library backup");
    }
    let source_root = native_path_from_location(&manifest.source_root)?;
    for location in &manifest.owned_locations {
        if !matches!(location.table.as_str(), "locations" | "library_sources")
            || location.id.len() != 16
        {
            bail!("backup contains an invalid owned location");
        }
        relative_path(&location.relative)?;
    }
    if !source_root.is_absolute() {
        bail!("backup source identity is not absolute");
    }
    let payload = root.join("payload");
    if fs::symlink_metadata(&payload)?.file_type().is_symlink() || !payload.is_dir() {
        bail!("backup payload is not a regular directory");
    }
    let mut seen = BTreeSet::new();
    let mut byte_count = 0u64;
    control.total.store(
        manifest.files.len() as u64,
        std::sync::atomic::Ordering::Release,
    );
    control
        .completed
        .store(0, std::sync::atomic::Ordering::Release);
    for entry in &manifest.files {
        control.check()?;
        entry.modified.time()?;
        let relative = relative_path(&entry.path)?;
        if !seen.insert(relative.clone()) {
            bail!("backup contains duplicate paths");
        }
        let path = payload.join(&relative);
        transfer::contained_file(&payload, &path)?;
        let (count, digest) = transfer::digest(&path, control)?;
        if count != entry.byte_count || digest != entry.sha256 {
            bail!("backup file failed verification: {}", relative.display());
        }
        if entry.sqlite {
            transfer::verify_database(&path, relative == Path::new("catalog.sqlite"))?;
        }
        byte_count = byte_count
            .checked_add(count)
            .context("backup size overflow")?;
        control
            .completed
            .fetch_add(1, std::sync::atomic::Ordering::Release);
    }
    let actual = transfer::payload_inventory(&payload)?;
    if actual.into_iter().collect::<BTreeSet<_>>() != seen || byte_count != manifest.byte_count {
        bail!("backup file set or size does not match its manifest");
    }
    if profile(&payload, "")? != manifest.profile {
        bail!("backup library marker changed");
    }
    if !seen.contains(Path::new("catalog.sqlite")) {
        bail!("backup has no desktop catalog");
    }
    shadow_catalog::verify_catalog_backup(&payload.join("catalog.sqlite"))?;
    Ok(manifest)
}

pub(super) fn file(
    path: &Path,
    byte_count: u64,
    sha256: String,
    sqlite: bool,
    modified: ArchiveTimestamp,
) -> ArchiveFile {
    ArchiveFile {
        path: native_location(path),
        byte_count,
        sha256,
        sqlite,
        modified,
    }
}
