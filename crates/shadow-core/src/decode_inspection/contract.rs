//! Provider-neutral decode inspection contracts and terminal accounting.

use std::path::{Path, PathBuf};

use shadow_cache::CacheError;
use shadow_catalog::{CatalogError, RepresentationFingerprint};
use shadow_domain::{
    DecoderSnapshot, ImageDimensions, PreviewPayload, ProxyPayload, RepresentationId,
};
use thiserror::Error;

use crate::performance::{DecodePerformance, TechnicalPerformance};
use crate::technical_observation::TechnicalObservationError;

/// Provider-neutral operation used by the background decode worker.
///
/// Implementations adapt `LibRaw`, a private vendor SDK bridge, a DNG converter,
/// or a test double without exposing provider types to the scheduler.
pub trait DecodeInspector: Send + 'static {
    /// Stable provider id used for Catalog cache reconciliation.
    #[allow(clippy::unnecessary_literal_bound)]
    fn provider_id(&self) -> &str {
        "anonymous"
    }

    /// Provider build/version used to invalidate stale capability results.
    #[allow(clippy::unnecessary_literal_bound)]
    fn provider_version(&self) -> &str {
        "1"
    }

    /// Returns whether this inspector can safely inspect an original RAW
    /// source.
    ///
    /// The conservative default is RAW-only. This preserves the current
    /// LibRaw-backed behavior. Raster formats use the separate, extension
    /// precise [`Self::supported_original_raster_extensions`] contract so an
    /// inspector that only understands JPEG is never sent a TIFF or PNG.
    fn supports_original_raw(&self) -> bool {
        true
    }

    /// Returns the lower-level raster file extensions this inspector can
    /// actually open, without the leading period.
    ///
    /// The empty default is deliberate: raster files remain discoverable in
    /// the catalog, but they are not submitted to a RAW-only inspector. A
    /// provider may report `jpg`/`jpeg`, and conditionally `heic`/`heif` when
    /// its optional HEIF backend is compiled in.
    fn supported_original_raster_extensions(&self) -> Vec<String> {
        Vec::new()
    }

    /// Inspects one source and returns an owned, provider-neutral snapshot.
    ///
    /// # Errors
    ///
    /// Returns a provider diagnostic suitable for the background job log.
    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String>;

    /// Extracts the provider-selected embedded preview when available.
    ///
    /// The default keeps descriptor-only inspectors valid. Implementations
    /// should return `Ok(None)` for a legitimate no-preview source.
    ///
    /// # Errors
    ///
    /// Returns a provider diagnostic when preview extraction fails.
    fn extract_best_preview(&mut self, _path: &Path) -> Result<Option<PreviewPayload>, String> {
        Ok(None)
    }

    /// Renders a bounded display proxy when no embedded preview is available.
    ///
    /// The variant key is stored in the Catalog so later renderer changes can
    /// invalidate only proxies produced by an older recipe.
    ///
    /// # Errors
    ///
    /// Returns a provider diagnostic when reference rendering or encoding fails.
    fn render_proxy(&mut self, _path: &Path) -> Result<Option<ProxyPayload>, String> {
        Ok(None)
    }

    #[allow(clippy::unnecessary_literal_bound)]
    fn proxy_variant_key(&self) -> &str {
        "anonymous:grid-jpeg-2048-q95-444-v1"
    }
}

impl<F> DecodeInspector for F
where
    F: FnMut(&Path) -> Result<DecoderSnapshot, String> + Send + 'static,
{
    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
        self(path)
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct DecodeInspectionRequest {
    pub representation_id: RepresentationId,
    pub path: PathBuf,
    pub expected_source: RepresentationFingerprint,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum DecodeInspectionDiscardReason {
    FilesystemChanged,
    CatalogChanged,
    Cancelled,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub enum DecodeInspectionOutcome {
    Recorded {
        provider_id: String,
        provider_version: String,
        preview: PreviewCacheOutcome,
    },
    Discarded(DecodeInspectionDiscardReason),
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub enum PreviewCacheOutcome {
    NotRequested,
    NoVisualAvailable,
    /// An embedded camera preview was handed to a session-owned visual sink.
    ///
    /// Unlike [`Self::StoredEmbeddedPreview`], this result intentionally has
    /// no cache blob or Catalog artifact. It is valid only while that desktop
    /// session keeps the preview in memory.
    PublishedEmbeddedPreview {
        byte_len: u64,
    },
    StoredEmbeddedPreview {
        digest_hex: String,
        byte_len: u64,
    },
    StoredGeneratedProxy {
        digest_hex: String,
        byte_len: u64,
        dimensions: ImageDimensions,
    },
    Discarded(DecodeInspectionDiscardReason),
    Failed(String),
}

/// Terminal accounting for every inspection accepted by one actor.
///
/// `completed` is the total number of accepted requests that reached a
/// terminal result. The remaining counters are diagnostic subsets of that
/// total: hard worker results, preview-only failures, and cooperative
/// cancellation respectively.
#[derive(Debug, Clone, Copy, Default, Eq, PartialEq)]
pub struct DecodeInspectionSummary {
    pub completed: u64,
    pub hard_failures: u64,
    pub preview_failures: u64,
    pub cancelled: u64,
}

/// A non-terminal observation of a decode inspection pool.
///
/// `summary` preserves the exact terminal accounting contract: a request only
/// contributes to it after its complete inspection has finished. Visual
/// publications are deliberately separate because an embedded camera preview
/// may become usable before the worker has finished generating Shadow's proxy.
#[derive(Debug, Clone, Copy, Default, Eq, PartialEq)]
pub struct DecodeInspectionProgress {
    pub summary: DecodeInspectionSummary,
    pub visual_artifacts_published: u64,
}

/// One session-scoped embedded preview publication.
///
/// The decode scheduler owns extraction timing, while the consumer owns
/// retention. This prevents camera-produced previews from becoming durable
/// Shadow cache artifacts while still allowing a Library card to appear
/// before deterministic proxy rendering has finished.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EmbeddedPreviewPublication {
    pub representation_id: RepresentationId,
    pub source: RepresentationFingerprint,
    pub preview: PreviewPayload,
}

/// Receives transient camera previews for the lifetime of a host session.
///
/// Implementations must not assume that the source will remain valid after
/// publication; consumers should use `representation_id` plus `source` as
/// their identity guard. Returning an error discards only this temporary
/// visual and never prevents generated-proxy rendering from continuing.
pub trait EmbeddedPreviewSink: Send + Sync {
    /// Publishes one embedded preview without creating a durable cache entry.
    ///
    /// # Errors
    ///
    /// Returns a session-local diagnostic when the consumer cannot retain the
    /// transient preview. Generated-proxy rendering may still continue.
    fn publish_embedded_preview(
        &self,
        publication: EmbeddedPreviewPublication,
    ) -> Result<(), String>;
}

/// Terminal accounting and optional phase aggregates for one decode actor.
///
/// Actors created with the default constructors return disabled, empty
/// performance aggregates while preserving the exact summary contract.
#[derive(Debug, Clone, Default, Eq, PartialEq)]
pub struct DecodeInspectionTerminal {
    pub summary: DecodeInspectionSummary,
    pub decode: DecodePerformance,
    pub technical: TechnicalPerformance,
}

impl DecodeInspectionSummary {
    pub(super) fn record(
        &mut self,
        result: &Result<DecodeInspectionOutcome, DecodeInspectionError>,
    ) {
        self.completed = self.completed.saturating_add(1);
        match result {
            Err(_) => self.hard_failures = self.hard_failures.saturating_add(1),
            Ok(
                DecodeInspectionOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled)
                | DecodeInspectionOutcome::Recorded {
                    preview:
                        PreviewCacheOutcome::Discarded(DecodeInspectionDiscardReason::Cancelled),
                    ..
                },
            ) => self.cancelled = self.cancelled.saturating_add(1),
            Ok(DecodeInspectionOutcome::Recorded {
                preview: PreviewCacheOutcome::Failed(_),
                ..
            }) => self.preview_failures = self.preview_failures.saturating_add(1),
            Ok(_) => {}
        }
    }
}

#[derive(Debug, Error)]
pub enum DecodeInspectionError {
    #[error("cannot read source metadata for {path}: {source}")]
    SourceMetadata {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("decoder inspection failed for {path}: {message}")]
    Inspector { path: PathBuf, message: String },
    #[error("catalog operation failed: {0}")]
    Catalog(#[from] CatalogError),
    #[error("preview cache operation failed: {0}")]
    Cache(#[from] CacheError),
    #[error("technical observation operation failed: {0}")]
    TechnicalObservation(#[from] TechnicalObservationError),
    #[error("cannot start decode inspection worker: {0}")]
    WorkerStart(#[source] std::io::Error),
    #[error("decode inspection pool requires at least one worker")]
    InvalidWorkerCount,
    #[error("cannot construct decode inspection worker {worker_index}: {message}")]
    WorkerFactory {
        worker_index: usize,
        message: String,
    },
    #[error("decode inspection worker {worker_index} has a different provider contract")]
    WorkerContractMismatch { worker_index: usize },
    #[error("decode inspection worker is unavailable")]
    WorkerUnavailable,
    #[error("decode inspection did not complete within {0:?}")]
    CompletionTimeout(std::time::Duration),
    #[error("decode inspection worker panicked")]
    WorkerPanicked,
}
