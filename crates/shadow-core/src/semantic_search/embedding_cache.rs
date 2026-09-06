//! Rebuildable image-vector cache. Exact source revision and embedding-space identity
//! isolate source replacement and model changes; malformed entries are cache misses.
use serde::{Deserialize, Serialize};
use std::io::Write;
use std::sync::atomic::{AtomicU64, Ordering};
use std::{
    fs,
    path::{Path, PathBuf},
};
static NEXT_WRITE: AtomicU64 = AtomicU64::new(0);
use shadow_ai::{SemanticEmbedding, SemanticEmbeddingSpace};

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Record {
    version: u32,
    source_revision: String,
    space: SemanticEmbeddingSpace,
    width: u32,
    height: u32,
    values: Vec<f32>,
}

pub(super) struct EmbeddingCache {
    root: PathBuf,
}
impl EmbeddingCache {
    pub(super) fn new(root: &Path) -> Self {
        Self {
            root: root.join("ai/semantic-search-v1"),
        }
    }
    fn path(&self, source: &str, space: &SemanticEmbeddingSpace) -> PathBuf {
        let mut hash = blake3::Hasher::new();
        hash.update(b"shadow-semantic-cache-v1\0");
        hash.update(source.as_bytes());
        hash.update(&[0]);
        hash.update(&serde_json::to_vec(space).unwrap_or_default());
        self.root.join(format!("{}.json", hash.finalize().to_hex()))
    }
    pub(super) fn load(
        &self,
        source: &str,
        space: &SemanticEmbeddingSpace,
        width: u32,
        height: u32,
    ) -> Option<SemanticEmbedding> {
        let path = self.path(source, space);
        if fs::symlink_metadata(&path).ok()?.file_type().is_symlink() {
            return None;
        }
        let file = fs::File::open(path).ok()?;
        if file.metadata().ok()?.len() > 128 * 1024 {
            return None;
        }
        let record: Record = serde_json::from_reader(std::io::Read::take(file, 128 * 1024)).ok()?;
        if record.version != 1
            || record.source_revision != source
            || &record.space != space
            || record.width != width
            || record.height != height
        {
            return None;
        }
        SemanticEmbedding::new(record.space, record.values).ok()
    }
    pub(super) fn store(
        &self,
        source: &str,
        embedding: &SemanticEmbedding,
        width: u32,
        height: u32,
    ) {
        let record = Record {
            version: 1,
            source_revision: source.into(),
            space: embedding.space().clone(),
            width,
            height,
            values: embedding.values().to_vec(),
        };
        let Ok(bytes) = serde_json::to_vec(&record) else {
            return;
        };
        if fs::create_dir_all(&self.root).is_err() {
            return;
        }
        // Cache failures never invalidate the search or become Catalog facts.
        let temporary = self.root.join(format!(
            ".{}-{}.tmp",
            std::process::id(),
            NEXT_WRITE.fetch_add(1, Ordering::Relaxed)
        ));
        let Ok(mut file) = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
        else {
            return;
        };
        if file.write_all(&bytes).is_ok() {
            drop(file);
            let _ = fs::rename(&temporary, self.path(source, embedding.space()));
        }
        let _ = fs::remove_file(temporary);
    }
}

#[cfg(test)]
mod tests;
