//! Bounded, in-memory camera previews for one desktop session.
//!
//! Embedded previews are useful for immediate Library feedback, but they are
//! neither Shadow renders nor durable cache material. This module deliberately
//! owns only transient encoded bytes; closing the desktop session drops every
//! entry without touching the Catalog or cache filesystem.

use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
};

use shadow_catalog::RepresentationFingerprint;
use shadow_core::{EmbeddedPreviewPublication, EmbeddedPreviewSink};
use shadow_domain::{ImageDimensions, PreviewByteOrder, PreviewCodec, RepresentationId};

/// Generous enough for several full-size camera JPEGs without allowing bulk
/// import to retain unbounded duplicate pixels beside the durable proxy cache.
const MAX_SESSION_PREVIEW_BYTES: usize = 256 * 1_024 * 1_024;

/// Metadata carried by a signed UI handle. The encoded bytes remain private to
/// the store and are never written into the Catalog.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) struct SessionPreviewDescriptor {
    pub(crate) representation_id: RepresentationId,
    pub(crate) source: RepresentationFingerprint,
    pub(crate) revision: u64,
    pub(crate) codec: PreviewCodec,
    pub(crate) byte_order: PreviewByteOrder,
    pub(crate) dimensions: ImageDimensions,
    pub(crate) bits_per_channel: u16,
    pub(crate) channels: u16,
}

#[derive(Debug)]
struct SessionPreview {
    descriptor: SessionPreviewDescriptor,
    bytes: Arc<[u8]>,
    last_used: u64,
}

#[derive(Debug, Default)]
struct SessionPreviewState {
    previews: HashMap<RepresentationId, SessionPreview>,
    retained_bytes: usize,
    next_revision: u64,
    next_access: u64,
}

/// A memory-bounded LRU of short-lived camera previews.
#[derive(Debug, Default)]
pub(crate) struct SessionPreviewStore {
    state: Mutex<SessionPreviewState>,
}

impl SessionPreviewStore {
    pub(crate) fn lookup(
        &self,
        representation_id: RepresentationId,
        source: RepresentationFingerprint,
    ) -> Option<SessionPreviewDescriptor> {
        let mut state = self.state.lock().ok()?;
        let access = next_tick(&mut state.next_access);
        let preview = state.previews.get_mut(&representation_id)?;
        if preview.descriptor.source != source {
            return None;
        }
        preview.last_used = access;
        Some(preview.descriptor)
    }

    pub(crate) fn load(&self, descriptor: SessionPreviewDescriptor) -> Option<Arc<[u8]>> {
        let mut state = self.state.lock().ok()?;
        let access = next_tick(&mut state.next_access);
        let preview = state.previews.get_mut(&descriptor.representation_id)?;
        if preview.descriptor != descriptor {
            return None;
        }
        preview.last_used = access;
        Some(Arc::clone(&preview.bytes))
    }

    #[cfg(test)]
    fn retained_bytes(&self) -> usize {
        self.state.lock().map_or(0, |state| state.retained_bytes)
    }

    fn insert(&self, publication: EmbeddedPreviewPublication) -> Result<(), String> {
        let byte_len = publication.preview.bytes.len();
        if byte_len == 0 {
            return Err("embedded preview has no encoded bytes".to_owned());
        }
        if byte_len > MAX_SESSION_PREVIEW_BYTES {
            return Err(format!(
                "embedded preview is {byte_len} bytes, above this session's {MAX_SESSION_PREVIEW_BYTES} byte memory limit"
            ));
        }
        let mut state = self
            .state
            .lock()
            .map_err(|_| "session preview store lock is poisoned".to_owned())?;
        let revision = next_tick(&mut state.next_revision);
        let access = next_tick(&mut state.next_access);
        let descriptor = SessionPreviewDescriptor {
            representation_id: publication.representation_id,
            source: publication.source,
            revision,
            codec: publication.preview.descriptor.codec,
            byte_order: publication.preview.byte_order,
            dimensions: publication.preview.descriptor.dimensions,
            bits_per_channel: publication.preview.descriptor.bits_per_channel,
            channels: publication.preview.descriptor.channels,
        };
        let previous = state.previews.insert(
            publication.representation_id,
            SessionPreview {
                descriptor,
                bytes: Arc::from(publication.preview.bytes),
                last_used: access,
            },
        );
        if let Some(previous) = previous {
            state.retained_bytes = state.retained_bytes.saturating_sub(previous.bytes.len());
        }
        state.retained_bytes = state.retained_bytes.saturating_add(byte_len);
        evict_until_within_budget(&mut state, publication.representation_id);
        Ok(())
    }
}

impl EmbeddedPreviewSink for SessionPreviewStore {
    fn publish_embedded_preview(
        &self,
        publication: EmbeddedPreviewPublication,
    ) -> Result<(), String> {
        self.insert(publication)
    }
}

fn next_tick(value: &mut u64) -> u64 {
    *value = value.saturating_add(1);
    *value
}

fn evict_until_within_budget(state: &mut SessionPreviewState, protected: RepresentationId) {
    while state.retained_bytes > MAX_SESSION_PREVIEW_BYTES && state.previews.len() > 1 {
        let victim = state
            .previews
            .iter()
            .filter(|(representation_id, _)| **representation_id != protected)
            .min_by_key(|(_, preview)| preview.last_used)
            .map(|(representation_id, _)| *representation_id);
        let Some(victim) = victim else {
            break;
        };
        if let Some(evicted) = state.previews.remove(&victim) {
            state.retained_bytes = state.retained_bytes.saturating_sub(evicted.bytes.len());
        }
    }
}

#[cfg(test)]
mod tests;
