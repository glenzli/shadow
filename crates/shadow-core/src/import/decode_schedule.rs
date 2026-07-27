//! Decode-output freshness checks and bounded inspection submission.

use std::path::Path;

use shadow_catalog::{
    CatalogHandle, RegisterAsset, RegisteredAsset, RegistrationStatus, RepresentationFingerprint,
};
use shadow_domain::RepresentationKind;

use crate::{DecodeInspectionHandle, DecodeInspectionRequest};

use super::{
    scan_contract::{ScanCancellation, ScanError},
    scan_session::ScanProfiler,
};

#[derive(Debug)]
pub(super) struct DecodeScheduler<'a> {
    pub(super) catalog: CatalogHandle,
    pub(super) inspections: &'a DecodeInspectionHandle,
}

impl DecodeScheduler<'_> {
    pub(super) fn schedule(
        &self,
        path: &Path,
        kind: RepresentationKind,
        request: &RegisterAsset,
        registered: RegisteredAsset,
        cancellation: &ScanCancellation,
        profiler: &mut ScanProfiler,
    ) -> Result<bool, ScanError> {
        if cancellation.is_cancelled() {
            return Ok(false);
        }
        // The scanner only discovers user-owned original files. A provider
        // must explicitly opt into each raster extension; LibRaw and anonymous
        // legacy inspectors remain RAW-only, while a JPEG-only provider never
        // receives TIFF/PNG/HEIF just because those files were imported.
        if !matches!(
            kind,
            RepresentationKind::OriginalRaw | RepresentationKind::OriginalRaster
        ) || !self.inspections.supports_source(kind, path)
            || registered.status == RegistrationStatus::NeedsRevalidation
        {
            return Ok(false);
        }
        let source = RepresentationFingerprint {
            byte_len: request.byte_len,
            modified_at_ms: request.modified_at_ms,
        };
        if profiler.measure_decode_current_query(|| {
            self.catalog.is_decode_output_current(
                registered.representation_id,
                self.inspections.provider_id(),
                self.inspections.provider_version(),
                source,
                self.inspections.caches_previews(),
                self.inspections.proxy_variant_key(),
                self.inspections.technical_preprocessing_version(),
            )
        })? {
            return Ok(false);
        }
        if cancellation.is_cancelled() {
            return Ok(false);
        }
        let submission = profiler.measure_decode_submit_wait(|| {
            self.inspections.submit_with_cancellation_observed(
                DecodeInspectionRequest {
                    representation_id: registered.representation_id,
                    path: path.to_path_buf(),
                    expected_source: source,
                },
                cancellation,
            )
        })?;
        profiler.record_decode_queue_full_events(submission.queue_full_events);
        drop(submission.ticket);
        Ok(true)
    }
}
