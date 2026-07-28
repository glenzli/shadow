//! Conservative inventory and garbage collection for the rebuildable blob tree.
//!
//! The Catalog owns product meaning and supplies the live digest snapshot.
//! This module owns filesystem classification, the publication grace period,
//! and the removal report. It never verifies payload bytes or decides which
//! product artifacts remain live.

use std::{
    collections::BTreeSet,
    fs,
    path::{Path, PathBuf},
    time::{Duration, SystemTime},
};

use crate::{
    ALGORITHM_DIRECTORY, BlobDigest, CacheError, ContentAddressedStore, DIGEST_BYTES,
    relative_blob_path,
};

/// A cache entry can be written immediately before its Catalog reference is
/// committed. A maintenance pass must never race that publication and delete
/// the still-in-flight blob. A later sweep reclaims genuinely stale entries.
const DEFAULT_SWEEP_GRACE_PERIOD: Duration = Duration::from_mins(5);

/// One canonical blob discovered under the live content-addressed tree.
///
/// Product-specific meaning such as "thumbnail" or "AI mask" belongs to the
/// Catalog. This record only reports what safely exists on disk.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CacheBlobEntry {
    pub digest: BlobDigest,
    pub byte_len: u64,
    pub relative_path: PathBuf,
}

/// A conservative live-cache inventory.
///
/// Entries that do not follow Shadow's canonical BLAKE3 layout are reported
/// separately and are never removed by
/// [`ContentAddressedStore::sweep_unreferenced`]. This keeps maintenance safe
/// around interrupted writes, future cache formats, and diagnostic files.
#[derive(Debug, Clone, Eq, PartialEq, Default)]
pub struct CacheInventory {
    pub blobs: Vec<CacheBlobEntry>,
    pub total_byte_len: u64,
    pub unknown_relative_paths: Vec<PathBuf>,
}

/// The result of one conservative cache garbage-collection pass.
///
/// Garbage collection is intentionally a two-party operation: the caller
/// supplies the Catalog-derived live digest set, while this owner only removes
/// canonical blobs absent from that set. Age alone never evicts a valid blob.
#[derive(Debug, Clone, Eq, PartialEq, Default)]
pub struct CacheSweepReport {
    pub inventory: CacheInventory,
    pub retained_blob_count: u64,
    /// Canonical blobs deliberately skipped because they were modified too
    /// recently to be safely classified against a concurrent Catalog write.
    pub recently_protected_blob_count: u64,
    /// Total payload bytes represented by [`Self::recently_protected_blob_count`].
    pub recently_protected_byte_len: u64,
    pub reclaimed: Vec<CacheBlobEntry>,
    pub reclaimed_byte_len: u64,
    pub dry_run: bool,
}

impl ContentAddressedStore {
    /// Enumerates canonical blobs without reading their payloads.
    ///
    /// Integrity verification remains opt-in because rehashing every preview
    /// during a routine quota check would make a large Library feel like it is
    /// constantly re-importing. Consumers verify selected blobs through
    /// [`Self::verify`] before use.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError::Io`] when the canonical tree cannot be enumerated
    /// or inspected.
    pub fn inventory(&self) -> Result<CacheInventory, CacheError> {
        let algorithm_root = self.root.join("blobs").join(ALGORITHM_DIRECTORY);
        let mut inventory = CacheInventory::default();
        collect_cache_inventory(self.root(), &algorithm_root, &mut inventory)?;
        inventory.blobs.sort_by_key(|entry| entry.digest);
        inventory
            .unknown_relative_paths
            .sort_by(|left, right| left.as_os_str().cmp(right.as_os_str()));
        Ok(inventory)
    }

    /// Removes canonical blobs that have no Catalog-provided live reference.
    ///
    /// The supplied `retained` set should come from a consistent Catalog
    /// snapshot. `dry_run` provides an exact non-destructive preview. Unknown
    /// filesystem entries are always preserved.
    ///
    /// Entries modified during the last five minutes are retained even when
    /// absent from the snapshot, protecting the interval between an atomic
    /// cache write and its durable Catalog reference.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError::Io`] when inventory metadata cannot be read or an
    /// unreferenced canonical blob cannot be removed.
    pub fn sweep_unreferenced(
        &self,
        retained: &BTreeSet<BlobDigest>,
        dry_run: bool,
    ) -> Result<CacheSweepReport, CacheError> {
        let inventory = self.inventory()?;
        self.sweep_inventory_at(
            &inventory,
            retained,
            dry_run,
            SystemTime::now(),
            DEFAULT_SWEEP_GRACE_PERIOD,
        )
    }

    fn sweep_inventory_at(
        &self,
        inventory: &CacheInventory,
        retained: &BTreeSet<BlobDigest>,
        dry_run: bool,
        now: SystemTime,
        grace_period: Duration,
    ) -> Result<CacheSweepReport, CacheError> {
        let mut report = CacheSweepReport {
            inventory: inventory.clone(),
            dry_run,
            ..CacheSweepReport::default()
        };
        for entry in &inventory.blobs {
            if retained.contains(&entry.digest) {
                report.retained_blob_count = report.retained_blob_count.saturating_add(1);
                continue;
            }
            let path = self.root.join(&entry.relative_path);
            if blob_is_within_grace_period(&path, now, grace_period)? {
                report.recently_protected_blob_count =
                    report.recently_protected_blob_count.saturating_add(1);
                report.recently_protected_byte_len = report
                    .recently_protected_byte_len
                    .saturating_add(entry.byte_len);
                continue;
            }
            if !dry_run {
                match fs::remove_file(&path) {
                    Ok(()) => {}
                    Err(source) if source.kind() == std::io::ErrorKind::NotFound => continue,
                    Err(source) => return Err(CacheError::Io { path, source }),
                }
            }
            report.reclaimed_byte_len = report.reclaimed_byte_len.saturating_add(entry.byte_len);
            report.reclaimed.push(entry.clone());
        }
        Ok(report)
    }
}

fn blob_is_within_grace_period(
    path: &Path,
    now: SystemTime,
    grace_period: Duration,
) -> Result<bool, CacheError> {
    let modified = fs::metadata(path)
        .map_err(|source| CacheError::Io {
            path: path.to_path_buf(),
            source,
        })?
        .modified()
        .map_err(|source| CacheError::Io {
            path: path.to_path_buf(),
            source,
        })?;
    // Clock correction can produce a future timestamp. Treat it as recent:
    // deleting a cache blob is never worth risking an active publication.
    Ok(now
        .duration_since(modified)
        .map_or(true, |age| age < grace_period))
}

fn collect_cache_inventory(
    root: &Path,
    algorithm_root: &Path,
    inventory: &mut CacheInventory,
) -> Result<(), CacheError> {
    if !algorithm_root.exists() {
        return Ok(());
    }
    let prefixes = fs::read_dir(algorithm_root).map_err(|source| CacheError::Io {
        path: algorithm_root.to_path_buf(),
        source,
    })?;
    for prefix_entry in prefixes {
        let prefix_entry = prefix_entry.map_err(|source| CacheError::Io {
            path: algorithm_root.to_path_buf(),
            source,
        })?;
        let prefix_path = prefix_entry.path();
        let prefix_type = prefix_entry.file_type().map_err(|source| CacheError::Io {
            path: prefix_path.clone(),
            source,
        })?;
        if !prefix_type.is_dir() {
            record_unknown_path(root, &prefix_path, inventory);
            continue;
        }
        let Some(prefix) = prefix_entry.file_name().to_str().map(str::to_owned) else {
            record_unknown_path(root, &prefix_path, inventory);
            continue;
        };
        let entries = fs::read_dir(&prefix_path).map_err(|source| CacheError::Io {
            path: prefix_path.clone(),
            source,
        })?;
        for entry in entries {
            let entry = entry.map_err(|source| CacheError::Io {
                path: prefix_path.clone(),
                source,
            })?;
            let path = entry.path();
            let file_type = entry.file_type().map_err(|source| CacheError::Io {
                path: path.clone(),
                source,
            })?;
            if !file_type.is_file() {
                record_unknown_path(root, &path, inventory);
                continue;
            }
            let Some(file_name) = entry.file_name().to_str().map(str::to_owned) else {
                record_unknown_path(root, &path, inventory);
                continue;
            };
            let Some(digest) = digest_from_path_components(&prefix, &file_name) else {
                record_unknown_path(root, &path, inventory);
                continue;
            };
            let expected_relative_path = relative_blob_path(digest);
            if root.join(&expected_relative_path) != path {
                record_unknown_path(root, &path, inventory);
                continue;
            }
            let byte_len = entry
                .metadata()
                .map_err(|source| CacheError::Io {
                    path: path.clone(),
                    source,
                })?
                .len();
            inventory.total_byte_len = inventory.total_byte_len.saturating_add(byte_len);
            inventory.blobs.push(CacheBlobEntry {
                digest,
                byte_len,
                relative_path: expected_relative_path,
            });
        }
    }
    Ok(())
}

fn record_unknown_path(root: &Path, path: &Path, inventory: &mut CacheInventory) {
    inventory
        .unknown_relative_paths
        .push(path.strip_prefix(root).unwrap_or(path).to_path_buf());
}

fn digest_from_path_components(prefix: &str, file_name: &str) -> Option<BlobDigest> {
    if prefix.len() != 2 || file_name.len() != 62 {
        return None;
    }
    let mut text = String::with_capacity(64);
    text.push_str(prefix);
    text.push_str(file_name);
    let mut bytes = [0_u8; DIGEST_BYTES];
    for (index, byte) in bytes.iter_mut().enumerate() {
        let offset = index * 2;
        *byte =
            (hex_nibble(text.as_bytes()[offset])? << 4) | hex_nibble(text.as_bytes()[offset + 1])?;
    }
    Some(BlobDigest::from_bytes(bytes))
}

fn hex_nibble(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

#[cfg(test)]
mod tests;
