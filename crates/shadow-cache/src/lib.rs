//! Rebuildable content-addressed blob storage for previews and render proxies.

use std::{
    collections::BTreeSet,
    fs::{self, File, OpenOptions},
    io::{self, BufReader, Read, Write},
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, SystemTime},
};

use thiserror::Error;

const ALGORITHM_DIRECTORY: &str = "b3";
const DIGEST_BYTES: usize = 32;
/// A cache entry can be written immediately before its Catalog reference is
/// committed.  A manual maintenance pass must never race that publication and
/// delete the still-in-flight blob.  The short grace period keeps the cache
/// rebuildable without making the active editor brittle; a later sweep will
/// reclaim genuinely stale entries.
const DEFAULT_SWEEP_GRACE_PERIOD: Duration = Duration::from_mins(5);
static TEMP_FILE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Error)]
pub enum CacheError {
    #[error("cache filesystem error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("cache blob failed BLAKE3 verification: {0}")]
    CorruptBlob(PathBuf),
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Ord, PartialOrd)]
pub struct BlobDigest([u8; DIGEST_BYTES]);

impl BlobDigest {
    pub const fn from_bytes(bytes: [u8; DIGEST_BYTES]) -> Self {
        Self(bytes)
    }

    pub const fn algorithm(self) -> &'static str {
        "blake3-256"
    }

    pub const fn as_bytes(&self) -> &[u8; DIGEST_BYTES] {
        &self.0
    }

    pub fn to_hex(self) -> String {
        blake3::Hash::from_bytes(self.0).to_hex().to_string()
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct StoredBlob {
    pub digest: BlobDigest,
    pub byte_len: u64,
    pub relative_path: PathBuf,
}

/// One canonical blob discovered under the live content-addressed tree.
///
/// The cache deliberately does not attach product-specific meaning such as
/// "thumbnail" or "AI mask" here. The Catalog owns those references; this
/// record only reports what safely exists on disk.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CacheBlobEntry {
    pub digest: BlobDigest,
    pub byte_len: u64,
    pub relative_path: PathBuf,
}

/// A conservative live-cache inventory.
///
/// Entries that do not follow Shadow's canonical BLAKE3 layout are reported
/// separately and are never removed by [`ContentAddressedStore::sweep_unreferenced`].
/// That makes cache maintenance safe in the presence of interrupted writes,
/// future cache formats, and manual diagnostics files.
#[derive(Debug, Clone, Eq, PartialEq, Default)]
pub struct CacheInventory {
    pub blobs: Vec<CacheBlobEntry>,
    pub total_byte_len: u64,
    pub unknown_relative_paths: Vec<PathBuf>,
}

/// The result of one conservative cache garbage-collection pass.
///
/// Cache garbage collection is intentionally a two-party operation: the
/// caller supplies the Catalog-derived live digest set, while the cache layer
/// only removes canonical blobs absent from that set. A valid blob must never
/// be evicted merely because its filesystem timestamp is old.
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

#[derive(Debug, Clone, Eq, PartialEq)]
pub enum QuarantineStatus {
    Missing,
    NotCorrupt,
    Quarantined { relative_path: PathBuf },
}

/// A cloneable handle to one cache root.
///
/// Blob paths are derived from content only:
/// `blobs/b3/<first-two-hex>/<remaining-hex>`. Extensions and source filenames
/// are deliberately excluded from identity.
#[derive(Debug, Clone)]
pub struct ContentAddressedStore {
    root: Arc<PathBuf>,
}

impl ContentAddressedStore {
    /// Creates the root and algorithm directory when absent.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError`] when the cache directory cannot be created.
    pub fn open(root: impl Into<PathBuf>) -> Result<Self, CacheError> {
        let root = root.into();
        let algorithm_root = root.join("blobs").join(ALGORITHM_DIRECTORY);
        fs::create_dir_all(&algorithm_root).map_err(|source| CacheError::Io {
            path: algorithm_root,
            source,
        })?;
        Ok(Self {
            root: Arc::new(root),
        })
    }

    pub fn root(&self) -> &Path {
        self.root.as_path()
    }

    /// Stores bytes atomically and returns their stable content identity.
    ///
    /// Existing blobs are verified before reuse. Temporary files live beside
    /// the destination so the final rename remains atomic on both platforms.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError`] for directory, write, rename, or integrity errors.
    pub fn put(&self, bytes: &[u8]) -> Result<StoredBlob, CacheError> {
        let digest = BlobDigest(*blake3::hash(bytes).as_bytes());
        let relative_path = relative_blob_path(digest);
        let destination = self.root.join(&relative_path);
        let parent = destination.parent().ok_or_else(|| CacheError::Io {
            path: destination.clone(),
            source: io::Error::new(
                io::ErrorKind::InvalidInput,
                "content-addressed blob path has no parent",
            ),
        })?;
        fs::create_dir_all(parent).map_err(|source| CacheError::Io {
            path: parent.to_path_buf(),
            source,
        })?;

        if destination.exists() {
            self.verify(digest)?;
            return stored_blob(digest, relative_path, bytes.len());
        }

        let temporary = temporary_path(&destination);
        let write_result = write_new_file(&temporary, bytes);
        if let Err(error) = write_result {
            let _ = fs::remove_file(&temporary);
            return Err(error);
        }

        if let Err(source) = fs::rename(&temporary, &destination) {
            let _ = fs::remove_file(&temporary);
            if destination.exists() {
                self.verify(digest)?;
            } else {
                return Err(CacheError::Io {
                    path: destination,
                    source,
                });
            }
        }

        stored_blob(digest, relative_path, bytes.len())
    }

    /// Re-hashes one blob before use or reuse.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError::CorruptBlob`] for a digest mismatch and an I/O
    /// error when the blob cannot be read.
    pub fn verify(&self, digest: BlobDigest) -> Result<(), CacheError> {
        let path = self.resolve(digest);
        let file = File::open(&path).map_err(|source| CacheError::Io {
            path: path.clone(),
            source,
        })?;
        let mut reader = BufReader::new(file);
        let mut hasher = blake3::Hasher::new();
        let mut buffer = vec![0_u8; 64 * 1_024].into_boxed_slice();
        loop {
            let count = reader.read(&mut buffer).map_err(|source| CacheError::Io {
                path: path.clone(),
                source,
            })?;
            if count == 0 {
                break;
            }
            hasher.update(&buffer[..count]);
        }
        if hasher.finalize().as_bytes() != digest.as_bytes() {
            return Err(CacheError::CorruptBlob(path));
        }
        Ok(())
    }

    /// Loads one blob and verifies its content identity before returning bytes.
    ///
    /// This is the safe entry point for lazy UI/cache consumers. A truncated or
    /// replaced file is reported as corruption instead of reaching an image
    /// decoder under the Catalog's trusted metadata.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError::CorruptBlob`] for a digest mismatch and an I/O
    /// error when the blob cannot be read.
    pub fn read_verified(&self, digest: BlobDigest) -> Result<Vec<u8>, CacheError> {
        let path = self.resolve(digest);
        let bytes = fs::read(&path).map_err(|source| CacheError::Io {
            path: path.clone(),
            source,
        })?;
        if blake3::hash(&bytes).as_bytes() != digest.as_bytes() {
            return Err(CacheError::CorruptBlob(path));
        }
        Ok(bytes)
    }

    /// Moves a still-corrupt blob out of the live digest tree so regenerated
    /// content can reclaim its canonical path.
    ///
    /// The bytes are retained under `quarantine/b3` for diagnosis or manual
    /// recovery. The method re-verifies immediately before the rename and never
    /// moves a valid blob.
    ///
    /// # Errors
    ///
    /// Returns an I/O error when verification or the recoverable rename fails.
    pub fn quarantine_corrupt(&self, digest: BlobDigest) -> Result<QuarantineStatus, CacheError> {
        match self.verify(digest) {
            Ok(()) => return Ok(QuarantineStatus::NotCorrupt),
            Err(CacheError::Io { source, .. }) if source.kind() == io::ErrorKind::NotFound => {
                return Ok(QuarantineStatus::Missing);
            }
            Err(CacheError::CorruptBlob(_)) => {}
            Err(error) => return Err(error),
        }

        let source_path = self.resolve(digest);
        let quarantine_root = self.root.join("quarantine").join(ALGORITHM_DIRECTORY);
        fs::create_dir_all(&quarantine_root).map_err(|source| CacheError::Io {
            path: quarantine_root.clone(),
            source,
        })?;
        let sequence = TEMP_FILE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let relative_path = Path::new("quarantine")
            .join(ALGORITHM_DIRECTORY)
            .join(format!(
                "{}.{}.{}.corrupt",
                digest.to_hex(),
                std::process::id(),
                sequence
            ));
        let destination = self.root.join(&relative_path);
        match fs::rename(&source_path, &destination) {
            Ok(()) => Ok(QuarantineStatus::Quarantined { relative_path }),
            Err(source) if source.kind() == io::ErrorKind::NotFound => {
                Ok(QuarantineStatus::Missing)
            }
            Err(source) => Err(CacheError::Io {
                path: source_path,
                source,
            }),
        }
    }

    pub fn resolve(&self, digest: BlobDigest) -> PathBuf {
        self.root.join(relative_blob_path(digest))
    }

    /// Enumerates canonical blobs without reading their payloads.
    ///
    /// Integrity verification remains opt-in because rehashing every preview
    /// during a routine quota check would make a large Library feel like it is
    /// constantly re-importing. Consumers can verify a selected blob through
    /// [`Self::verify`] before use.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError::Io`] when the canonical cache tree cannot be
    /// enumerated or inspected.
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

    /// Removes canonical blobs which no longer have a Catalog-provided live
    /// reference.
    ///
    /// The supplied `retained` set should be read from a consistent Catalog
    /// snapshot. `dry_run` provides an exact, non-destructive preview for the
    /// cache settings UI and for tests. Unknown filesystem entries are never
    /// removed by this method.
    ///
    /// Entries modified during the last five minutes are retained even when
    /// absent from the snapshot. This guards the short interval between an
    /// atomic cache-file write and its durable Catalog reference. The report
    /// exposes those skipped entries so a future settings UI can explain why a
    /// subsequent maintenance pass may reclaim additional space.
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
                    Err(source) if source.kind() == io::ErrorKind::NotFound => continue,
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
    // A future timestamp can be caused by clock correction. Treat it as
    // recent, because deleting a cache blob is never worth risking an active
    // write or a surprising filesystem timestamp.
    Ok(now
        .duration_since(modified)
        .map_or(true, |age| age < grace_period))
}

fn stored_blob(
    digest: BlobDigest,
    relative_path: PathBuf,
    byte_len: usize,
) -> Result<StoredBlob, CacheError> {
    let byte_len = u64::try_from(byte_len).map_err(|source| CacheError::Io {
        path: relative_path.clone(),
        source: io::Error::new(io::ErrorKind::InvalidData, source),
    })?;
    Ok(StoredBlob {
        digest,
        byte_len,
        relative_path,
    })
}

fn relative_blob_path(digest: BlobDigest) -> PathBuf {
    let hex = digest.to_hex();
    Path::new("blobs")
        .join(ALGORITHM_DIRECTORY)
        .join(&hex[..2])
        .join(&hex[2..])
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

fn temporary_path(destination: &Path) -> PathBuf {
    let sequence = TEMP_FILE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let file_name = destination
        .file_name()
        .unwrap_or_else(|| std::ffi::OsStr::new("blob"))
        .to_string_lossy();
    destination.with_file_name(format!(
        ".{file_name}.{}.{}.tmp",
        std::process::id(),
        sequence
    ))
}

fn write_new_file(path: &Path, bytes: &[u8]) -> Result<(), CacheError> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|source| CacheError::Io {
            path: path.to_path_buf(),
            source,
        })?;
    file.write_all(bytes).map_err(|source| CacheError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    file.flush().map_err(|source| CacheError::Io {
        path: path.to_path_buf(),
        source,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn duplicate_content_reuses_one_verified_blob() {
        let root = fixture_root("reuse");
        let store = ContentAddressedStore::open(&root).expect("open cache");
        let first = store.put(b"preview bytes").expect("first put");
        let second = store.put(b"preview bytes").expect("second put");

        assert_eq!(first, second);
        assert_eq!(
            fs::read(store.resolve(first.digest)).expect("read blob"),
            b"preview bytes"
        );
        assert_eq!(first.digest.to_hex().len(), 64);
        fs::remove_dir_all(root).expect("remove fixture");
    }

    #[test]
    fn corrupt_existing_blob_is_never_silently_reused() {
        let root = fixture_root("corrupt");
        let store = ContentAddressedStore::open(&root).expect("open cache");
        let blob = store.put(b"correct").expect("put blob");
        fs::write(store.resolve(blob.digest), b"corrupt").expect("corrupt blob");

        assert!(matches!(
            store.put(b"correct"),
            Err(CacheError::CorruptBlob(_))
        ));
        fs::remove_dir_all(root).expect("remove fixture");
    }

    #[test]
    fn lazy_read_verifies_bytes_before_returning_them() {
        let root = fixture_root("verified-read");
        let store = ContentAddressedStore::open(&root).expect("open cache");
        let blob = store.put(b"display proxy").expect("put blob");
        assert_eq!(
            store.read_verified(blob.digest).expect("verified read"),
            b"display proxy"
        );

        fs::write(store.resolve(blob.digest), b"tampered proxy").expect("tamper blob");
        assert!(matches!(
            store.read_verified(blob.digest),
            Err(CacheError::CorruptBlob(_))
        ));
        fs::remove_dir_all(root).expect("remove fixture");
    }

    #[test]
    fn corrupt_blob_is_quarantined_and_can_be_regenerated() {
        let root = fixture_root("quarantine");
        let store = ContentAddressedStore::open(&root).expect("open cache");
        let blob = store.put(b"correct proxy").expect("put blob");
        fs::write(store.resolve(blob.digest), b"corrupt proxy").expect("corrupt blob");

        let status = store
            .quarantine_corrupt(blob.digest)
            .expect("quarantine corrupt blob");
        let QuarantineStatus::Quarantined { relative_path } = status else {
            panic!("expected quarantined blob, found {status:?}");
        };
        assert!(!store.resolve(blob.digest).exists());
        assert_eq!(
            fs::read(root.join(relative_path)).expect("read quarantined bytes"),
            b"corrupt proxy"
        );

        let regenerated = store.put(b"correct proxy").expect("regenerate blob");
        assert_eq!(regenerated.digest, blob.digest);
        assert_eq!(
            store
                .read_verified(regenerated.digest)
                .expect("read regenerated blob"),
            b"correct proxy"
        );
        fs::remove_dir_all(root).expect("remove fixture");
    }

    #[test]
    fn inventory_and_sweep_keep_catalog_references_and_preserve_unknown_files() {
        let root = fixture_root("sweep");
        let store = ContentAddressedStore::open(&root).expect("open cache");
        let retained = store.put(b"current recipe preview").expect("put retained");
        let stale = store.put(b"superseded thumbnail").expect("put stale");
        let diagnostics = root.join("blobs").join(ALGORITHM_DIRECTORY).join("notes");
        fs::create_dir_all(&diagnostics).expect("create diagnostics directory");
        let unknown = diagnostics.join("keep-for-diagnosis.txt");
        fs::write(&unknown, b"not a canonical cache blob").expect("write unknown file");

        let inventory = store.inventory().expect("inventory");
        assert_eq!(inventory.blobs.len(), 2);
        assert_eq!(inventory.total_byte_len, retained.byte_len + stale.byte_len);
        assert_eq!(inventory.unknown_relative_paths.len(), 1);

        let retained_digests = BTreeSet::from([retained.digest]);
        let protected = store
            .sweep_unreferenced(&retained_digests, true)
            .expect("recent blobs are protected from a default dry sweep");
        assert!(protected.dry_run);
        assert_eq!(protected.reclaimed.len(), 0);
        assert_eq!(protected.recently_protected_blob_count, 1);
        assert_eq!(protected.recently_protected_byte_len, stale.byte_len);

        let dry_run = store
            .sweep_inventory_at(
                &inventory,
                &retained_digests,
                true,
                SystemTime::now() + DEFAULT_SWEEP_GRACE_PERIOD + Duration::from_secs(1),
                DEFAULT_SWEEP_GRACE_PERIOD,
            )
            .expect("dry sweep after the explicit grace window");
        assert!(dry_run.dry_run);
        assert_eq!(dry_run.retained_blob_count, 1);
        assert_eq!(dry_run.reclaimed.len(), 1);
        assert_eq!(dry_run.reclaimed[0].digest, stale.digest);
        assert_eq!(dry_run.reclaimed[0].byte_len, stale.byte_len);
        assert!(store.resolve(stale.digest).exists());

        let sweep = store
            .sweep_inventory_at(
                &inventory,
                &retained_digests,
                false,
                SystemTime::now() + DEFAULT_SWEEP_GRACE_PERIOD + Duration::from_secs(1),
                DEFAULT_SWEEP_GRACE_PERIOD,
            )
            .expect("sweep after the explicit grace window");
        assert!(!sweep.dry_run);
        assert_eq!(sweep.reclaimed.len(), 1);
        assert_eq!(sweep.reclaimed[0].digest, stale.digest);
        assert!(store.resolve(retained.digest).exists());
        assert!(!store.resolve(sweep.reclaimed[0].digest).exists());
        assert!(unknown.exists());
        fs::remove_dir_all(root).expect("remove fixture");
    }

    fn fixture_root(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "shadow-cache-{name}-{}-{}",
            std::process::id(),
            TEMP_FILE_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ))
    }
}
