//! Bounded session-local reuse of an already admitted candidate, not a promise
//! to select the Runtime's latest deployment. Explicit Refresh always bypasses it.
//! Accepted edits live independently in the durable managed raster store.

use super::ImageCompletionInvocation;
use shadow_ai::InferImageCompletionEvidence;
use std::{collections::VecDeque, sync::Mutex};
const MAX_BYTES: usize = 8 * 1024 * 1024;

#[derive(Debug, Default)]
pub(super) struct CompletionRequestCache {
    entries: Mutex<VecDeque<([u8; 32], InferImageCompletionEvidence)>>,
}
impl CompletionRequestCache {
    pub(super) fn key(input: &ImageCompletionInvocation) -> [u8; 32] {
        let mut hash = blake3::Hasher::new();
        hash.update(b"shadow-completion-candidate-reuse-v1");
        for bytes in [
            input.prepared_crop_png.as_slice(),
            input.prepared_mask_png.as_slice(),
            input.prepared_mask_gray8.as_slice(),
            input.source_recipe_blake3.as_bytes(),
            input.photo_id.as_bytes(),
        ] {
            hash.update(&(bytes.len() as u64).to_le_bytes());
            hash.update(bytes);
        }
        hash.update(&input.coordinate_extent.width.to_le_bytes());
        hash.update(&input.coordinate_extent.height.to_le_bytes());
        hash.update(&[u8::from(input.scene_referred_input)]);
        *hash.finalize().as_bytes()
    }
    pub(super) fn get(&self, key: &[u8; 32]) -> Option<InferImageCompletionEvidence> {
        let mut entries = self.entries.lock().ok()?;
        let index = entries.iter().position(|(k, _)| k == key)?;
        let entry = entries.remove(index)?;
        let evidence = entry.1.clone();
        entries.push_back(entry);
        Some(evidence)
    }
    pub(super) fn insert(&self, key: [u8; 32], evidence: InferImageCompletionEvidence) {
        if evidence.rgb8.is_empty() || evidence.rgb8.len() > MAX_BYTES {
            return;
        }
        let Ok(mut entries) = self.entries.lock() else {
            return;
        };
        entries.retain(|(k, _)| *k != key);
        while entries.len() >= 8
            || entries.iter().map(|(_, v)| v.rgb8.len()).sum::<usize>() + evidence.rgb8.len()
                > MAX_BYTES
        {
            entries.pop_front();
        }
        entries.push_back((key, evidence));
    }
}

#[cfg(test)]
mod tests;
