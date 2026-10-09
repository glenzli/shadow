//! Bounded reuse of validated immutable Recipe JSON decoding.
//!
//! This owns no durable authority. Callers supply current database bytes and
//! still validate indexed identity/parent edges and repair the current digest.
//! The byte budget counts retained JSON, not the decoded values' total heap use.

use std::collections::{HashMap, VecDeque};

use shadow_domain::{PhotoId, RecipeCommit, RecipeCommitId};

const MAX_ENTRIES: usize = 2_048;
const MAX_JSON_BYTES: usize = 32 * 1024 * 1024;
const MAX_ENTRY_JSON_BYTES: usize = 256 * 1024;

type Key = (PhotoId, RecipeCommitId);

#[derive(Debug, Clone)]
pub(super) struct ValidatedRecipe {
    pub(super) commit: RecipeCommit,
    pub(super) snapshot_digest: [u8; 32],
}

#[derive(Debug)]
struct Entry {
    json: Box<str>,
    value: ValidatedRecipe,
}

/// FIFO admission keeps hit lookup constant-time and bounds retained entries.
/// Recipes exceeding the admission budget remain readable through normal decode.
/// A sequential scan larger than this cache can evict all prior entries; this
/// deliberately bounded optimization does not promise hits for such histories.
#[derive(Debug)]
pub(crate) struct DecodedRecipeCache {
    entries: HashMap<Key, Entry>,
    insertion_order: VecDeque<Key>,
    json_bytes: usize,
    max_entries: usize,
    max_json_bytes: usize,
    max_entry_json_bytes: usize,
}

impl Default for DecodedRecipeCache {
    fn default() -> Self {
        Self::with_limits(MAX_ENTRIES, MAX_JSON_BYTES, MAX_ENTRY_JSON_BYTES)
    }
}

impl DecodedRecipeCache {
    fn with_limits(max_entries: usize, max_json_bytes: usize, max_entry_json_bytes: usize) -> Self {
        Self {
            entries: HashMap::new(),
            insertion_order: VecDeque::new(),
            json_bytes: 0,
            max_entries,
            max_json_bytes,
            max_entry_json_bytes,
        }
    }

    pub(super) fn get(
        &self,
        photo_id: PhotoId,
        commit_id: RecipeCommitId,
        json: &str,
    ) -> Option<ValidatedRecipe> {
        self.entries
            .get(&(photo_id, commit_id))
            .filter(|entry| entry.json.as_ref() == json)
            .map(|entry| entry.value.clone())
    }

    pub(super) fn insert(
        &mut self,
        photo_id: PhotoId,
        json: &str,
        commit: &RecipeCommit,
        snapshot_digest: [u8; 32],
    ) {
        let key = (photo_id, commit.id());
        if let Some(old) = self.entries.remove(&key) {
            self.json_bytes -= old.json.len();
            self.insertion_order.retain(|old_key| *old_key != key);
        }
        if self.max_entries == 0
            || json.len() > self.max_json_bytes
            || json.len() > self.max_entry_json_bytes
        {
            return;
        }
        while self.entries.len() >= self.max_entries
            || self.json_bytes > self.max_json_bytes - json.len()
        {
            let oldest = self
                .insertion_order
                .pop_front()
                .expect("cached entries have insertion order");
            let old = self
                .entries
                .remove(&oldest)
                .expect("insertion order names a cached entry");
            self.json_bytes -= old.json.len();
        }
        self.json_bytes += json.len();
        self.insertion_order.push_back(key);
        self.entries.insert(
            key,
            Entry {
                json: json.into(),
                value: ValidatedRecipe {
                    commit: commit.clone(),
                    snapshot_digest,
                },
            },
        );
    }
}

#[cfg(test)]
mod tests;
