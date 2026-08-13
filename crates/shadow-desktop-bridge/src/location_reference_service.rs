//! Durable, read-only location anchors from a user-selected reference library.
//!
//! This service deliberately keeps reference photos out of Shadow's primary
//! Catalog. A manifest contains only lossless root identities plus the small,
//! rebuildable `{capture time, GPS}` anchor index needed by location completion.

use std::{
    fs,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, anyhow, bail};
use serde::{Deserialize, Serialize};
use shadow_core::DecodeInspector;
use shadow_domain::AssetLocation;
use shadow_native_path::native_location;

use crate::PhotoInspector;

const MANIFEST_REVISION: u32 = 1;
const MANIFEST_NAME: &str = "location-reference-anchors.json";

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub(crate) struct LocationReferenceLibrary {
    pub id: String,
    pub root: AssetLocation,
    pub clock_offset_seconds: i64,
    pub indexed_at_unix_ms: i64,
    pub anchor_count: u64,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub(crate) struct LocationReferenceAnchor {
    pub library_id: String,
    pub relative_path: String,
    pub captured_at_unix_seconds: i64,
    pub latitude_e7: i32,
    pub longitude_e7: i32,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct LocationReferenceSnapshot {
    pub libraries: Vec<LocationReferenceLibrary>,
    pub anchors: Vec<LocationReferenceAnchor>,
}

#[derive(Debug, Serialize, Deserialize)]
struct Manifest {
    revision: u32,
    libraries: Vec<LocationReferenceLibrary>,
    anchors: Vec<LocationReferenceAnchor>,
}

#[derive(Debug)]
pub(crate) struct LocationReferenceService {
    manifest_path: PathBuf,
    manifest: Manifest,
}

impl LocationReferenceService {
    pub(crate) fn open(root: PathBuf) -> Result<Self> {
        fs::create_dir_all(&root)
            .with_context(|| format!("create location reference directory {}", root.display()))?;
        let manifest_path = root.join(MANIFEST_NAME);
        let manifest = load_manifest(&manifest_path)?;
        Ok(Self {
            manifest_path,
            manifest,
        })
    }

    #[must_use]
    pub(crate) fn snapshot(&self) -> LocationReferenceSnapshot {
        LocationReferenceSnapshot {
            libraries: self.manifest.libraries.clone(),
            anchors: self.manifest.anchors.clone(),
        }
    }

    /// Rebuilds one reference root's lightweight anchor index.
    ///
    /// A reference image is never added to the Catalog and no image pixels are
    /// cached. Decode failures and non-geotagged files are skipped; only an
    /// unreadable root is an operation error.
    pub(crate) fn add_or_rescan(
        &mut self,
        id: &str,
        root: &Path,
        clock_offset_seconds: i64,
        indexed_at_unix_ms: i64,
    ) -> Result<LocationReferenceLibrary> {
        let mut inspector = PhotoInspector::new_with_isolated_proxy_cache(None)?;
        self.add_or_rescan_with(id, root, clock_offset_seconds, indexed_at_unix_ms, |path| {
            inspector.inspect(path)
        })
    }

    /// Rebuilds a reference root using a stable identity derived from its
    /// lossless native location. Re-adding the same folder therefore refreshes
    /// its anchors instead of creating a second reference library.
    pub(crate) fn add_or_rescan_root(
        &mut self,
        root: &Path,
        clock_offset_seconds: i64,
        indexed_at_unix_ms: i64,
    ) -> Result<LocationReferenceLibrary> {
        let root = canonical_root(root)?;
        let id = library_id_for_root(&root)?;
        self.add_or_rescan(&id, &root, clock_offset_seconds, indexed_at_unix_ms)
    }

    pub(crate) fn add_or_rescan_with(
        &mut self,
        id: &str,
        root: &Path,
        clock_offset_seconds: i64,
        indexed_at_unix_ms: i64,
        inspect: impl FnMut(&Path) -> std::result::Result<shadow_domain::DecoderSnapshot, String>,
    ) -> Result<LocationReferenceLibrary> {
        let id = normalized_id(id)?;
        let root = canonical_root(root)?;
        let anchors = collect_anchors(&id, &root, clock_offset_seconds, inspect)?;
        let library = LocationReferenceLibrary {
            id: id.clone(),
            root: native_location(&root),
            clock_offset_seconds,
            indexed_at_unix_ms,
            anchor_count: u64::try_from(anchors.len()).unwrap_or(u64::MAX),
        };
        self.manifest
            .libraries
            .retain(|candidate| candidate.id != id);
        self.manifest
            .anchors
            .retain(|anchor| anchor.library_id != id);
        self.manifest.libraries.push(library.clone());
        self.manifest.anchors.extend(anchors);
        self.manifest
            .libraries
            .sort_by(|left, right| left.id.cmp(&right.id));
        self.manifest.anchors.sort_by(|left, right| {
            left.library_id
                .cmp(&right.library_id)
                .then_with(|| {
                    left.captured_at_unix_seconds
                        .cmp(&right.captured_at_unix_seconds)
                })
                .then_with(|| left.relative_path.cmp(&right.relative_path))
        });
        persist_manifest(&self.manifest_path, &self.manifest)?;
        Ok(library)
    }

    pub(crate) fn remove(&mut self, id: &str) -> Result<bool> {
        let id = normalized_id(id)?;
        let mut libraries = std::mem::take(&mut self.manifest.libraries);
        let before = libraries.len();
        libraries.retain(|library| library.id != id);
        self.manifest.libraries = libraries;
        if before == self.manifest.libraries.len() {
            return Ok(false);
        }
        self.manifest
            .anchors
            .retain(|anchor| anchor.library_id != id);
        persist_manifest(&self.manifest_path, &self.manifest)?;
        Ok(true)
    }
}

fn normalized_id(value: &str) -> Result<String> {
    let value = value.trim();
    if value.is_empty() || value.len() > 160 {
        bail!("reference library id must contain 1 to 160 characters");
    }
    if !value
        .chars()
        .all(|character| character.is_ascii_alphanumeric() || character == '-' || character == '_')
    {
        bail!("reference library id may only contain ASCII letters, digits, '-' and '_'");
    }
    Ok(value.to_owned())
}

fn library_id_for_root(root: &Path) -> Result<String> {
    let encoded = serde_json::to_vec(&native_location(root))
        .context("serialize reference library root identity")?;
    let digest = blake3::hash(&encoded).to_hex();
    Ok(format!("reference-{}", &digest[..16]))
}

fn canonical_root(root: &Path) -> Result<PathBuf> {
    let root = root
        .canonicalize()
        .with_context(|| format!("open reference library {}", root.display()))?;
    if !root.is_dir() {
        bail!("reference library {} is not a directory", root.display());
    }
    Ok(root)
}

fn collect_anchors(
    library_id: &str,
    root: &Path,
    clock_offset_seconds: i64,
    mut inspect: impl FnMut(&Path) -> std::result::Result<shadow_domain::DecoderSnapshot, String>,
) -> Result<Vec<LocationReferenceAnchor>> {
    let mut files = Vec::new();
    collect_files(root, &mut files)?;
    let mut anchors = Vec::new();
    for path in files {
        let Ok(snapshot) = inspect(&path) else {
            continue;
        };
        let Some(gps) = snapshot.metadata.gps else {
            continue;
        };
        let captured_at = snapshot.metadata.captured_at_unix_seconds;
        if captured_at <= 0
            || !gps.latitude_degrees.is_finite()
            || !gps.longitude_degrees.is_finite()
        {
            continue;
        }
        let Some(relative_path) = path
            .strip_prefix(root)
            .ok()
            .and_then(|relative| relative.to_str())
            .map(str::to_owned)
        else {
            continue;
        };
        let adjusted_capture = captured_at
            .checked_add(clock_offset_seconds)
            .ok_or_else(|| anyhow!("reference capture time overflow"))?;
        anchors.push(LocationReferenceAnchor {
            library_id: library_id.to_owned(),
            relative_path,
            captured_at_unix_seconds: adjusted_capture,
            latitude_e7: degrees_e7(gps.latitude_degrees, 90.0)?,
            longitude_e7: degrees_e7(gps.longitude_degrees, 180.0)?,
        });
    }
    Ok(anchors)
}

fn collect_files(root: &Path, files: &mut Vec<PathBuf>) -> Result<()> {
    for entry in fs::read_dir(root)
        .with_context(|| format!("read reference directory {}", root.display()))?
    {
        let entry =
            entry.with_context(|| format!("enumerate reference directory {}", root.display()))?;
        let file_type = entry
            .file_type()
            .with_context(|| format!("read reference entry {}", entry.path().display()))?;
        if file_type.is_symlink() {
            continue;
        }
        if file_type.is_dir() {
            collect_files(&entry.path(), files)?;
        } else if file_type.is_file() {
            files.push(entry.path());
        }
    }
    Ok(())
}

fn degrees_e7(value: f64, maximum: f64) -> Result<i32> {
    if !(-maximum..=maximum).contains(&value) {
        bail!("reference GPS coordinate is outside its legal range");
    }
    let scaled = (value * 10_000_000.0).round();
    i32::try_from(scaled as i64).context("reference GPS coordinate does not fit E7")
}

fn load_manifest(path: &Path) -> Result<Manifest> {
    if !path.exists() {
        return Ok(Manifest {
            revision: MANIFEST_REVISION,
            libraries: Vec::new(),
            anchors: Vec::new(),
        });
    }
    let bytes = fs::read(path)
        .with_context(|| format!("read location reference manifest {}", path.display()))?;
    let manifest: Manifest = serde_json::from_slice(&bytes)
        .with_context(|| format!("parse location reference manifest {}", path.display()))?;
    if manifest.revision != MANIFEST_REVISION {
        bail!(
            "unsupported location reference manifest revision {}",
            manifest.revision
        );
    }
    Ok(manifest)
}

fn persist_manifest(path: &Path, manifest: &Manifest) -> Result<()> {
    let bytes =
        serde_json::to_vec_pretty(manifest).context("serialize location reference manifest")?;
    let temporary = path.with_extension("json.tmp");
    fs::write(&temporary, bytes)
        .with_context(|| format!("write location reference manifest {}", temporary.display()))?;
    fs::rename(&temporary, path)
        .with_context(|| format!("publish location reference manifest {}", path.display()))
}

#[cfg(test)]
mod tests;
