//! Desktop-session delegation for AI RAW foundation jobs.
//!
//! These methods intentionally stop at model availability and verified cache
//! materialization. Recipe authoring remains a separate compare-and-swap edit
//! transaction, so cancellation or model failure can never mutate the user's
//! persistent enable/disable intent.

use anyhow::{Result as AnyResult, anyhow, bail};
use shadow_ai::{CancellationToken, RawFoundationMaterializationDisposition};
use shadow_domain::PhotoId;

use super::{
    DesktopSession, ffi,
    raw_foundation_runtime::{RawFoundationInvocation, RawFoundationReady},
    raw_foundation_service::{RawFoundationJobPhase, RawFoundationJobSnapshot},
    session_photo_source::catalog_native_path,
};

impl DesktopSession {
    pub(crate) fn probe_raw_foundation_runtime(&self) -> ffi::FfiRawFoundationRuntimeStatus {
        match self
            .raw_foundation_runtime
            .probe(&CancellationToken::default())
        {
            Ok(availability) => ffi::FfiRawFoundationRuntimeStatus {
                available: true,
                model_id: availability.model_id,
                runtime_version: availability.runtime_version,
                diagnostic: String::new(),
            },
            Err(error) => ffi::FfiRawFoundationRuntimeStatus {
                available: false,
                model_id: String::new(),
                runtime_version: String::new(),
                diagnostic: error.to_string(),
            },
        }
    }

    pub(crate) fn begin_raw_foundation_job(
        &self,
        request_id: &str,
        generation: u64,
    ) -> AnyResult<u64> {
        Ok(self
            .raw_foundations
            .begin_job(request_id.to_owned(), generation)?)
    }

    pub(crate) fn cancel_raw_foundation_job(&self, job_token: u64) -> AnyResult<()> {
        Ok(self.raw_foundations.cancel_job(job_token)?)
    }

    pub(crate) fn raw_foundation_job_status(
        &self,
        job_token: u64,
    ) -> AnyResult<ffi::FfiRawFoundationJobStatus> {
        self.ffi_raw_foundation_status(job_token)
    }

    pub(crate) fn execute_raw_foundation_job(
        &self,
        job_token: u64,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<ffi::FfiRawFoundationJobStatus> {
        let registered = self.raw_foundations.snapshot(job_token)?;
        let native_path = match self.validated_raw_foundation_source(photo_id, source_path) {
            Ok(native_path) => native_path,
            Err(error) => {
                self.raw_foundations
                    .complete_preflight_failure(job_token, error.to_string())?;
                return self.ffi_raw_foundation_status(job_token);
            }
        };
        self.raw_foundations.execute_job(
            &self.raw_foundation_runtime,
            job_token,
            RawFoundationInvocation {
                request_id: registered.request_id,
                generation: registered.generation,
                photo_id: photo_id.to_owned(),
                input_raw: native_path,
            },
        )?;
        self.ffi_raw_foundation_status(job_token)
    }

    pub(crate) fn retire_raw_foundation_job(&self, job_token: u64) -> AnyResult<()> {
        Ok(self.raw_foundations.retire_job(job_token)?)
    }

    fn ffi_raw_foundation_status(
        &self,
        job_token: u64,
    ) -> AnyResult<ffi::FfiRawFoundationJobStatus> {
        let snapshot = self.raw_foundations.snapshot(job_token)?;
        let ready = self.raw_foundations.ready_foundation(job_token)?;
        if snapshot.phase == RawFoundationJobPhase::Ready && ready.is_none() {
            return Err(anyhow!(
                "ready RAW foundation job has no verified descriptor"
            ));
        }
        Ok(ffi_job_status(snapshot, ready.as_ref()))
    }

    fn validated_raw_foundation_source(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<std::path::PathBuf> {
        let photo_id: PhotoId = photo_id.parse()?;
        let source = self
            .catalog
            .review_source(photo_id)?
            .ok_or_else(|| anyhow!("photo {photo_id} has no online original RAW source"))?;
        if source.location.display_path != source_path {
            bail!(
                "RAW source path does not belong to photo {photo_id}: expected {}, received {source_path}",
                source.location.display_path
            );
        }
        catalog_native_path(&source)
    }
}

fn ffi_job_status(
    snapshot: RawFoundationJobSnapshot,
    ready: Option<&RawFoundationReady>,
) -> ffi::FfiRawFoundationJobStatus {
    let (cache_key_sha256, artifact_identity_sha256, width, height) =
        ready.map_or_else(empty_ready_projection, |ready| {
            let extent = ready.descriptor.raster_extent();
            (
                ready.descriptor.provenance().cache_key_sha256().to_owned(),
                ready
                    .descriptor
                    .provenance()
                    .artifact_identity_sha256()
                    .to_owned(),
                extent.width,
                extent.height,
            )
        });
    ffi::FfiRawFoundationJobStatus {
        job_token: snapshot.token,
        request_id: snapshot.request_id,
        generation: snapshot.generation,
        phase: ffi_phase(snapshot.phase),
        phase_code: snapshot.phase_code,
        completed_basis_points: snapshot.completed_basis_points,
        cancellation_requested: snapshot.cancellation_requested,
        disposition: snapshot.disposition.map_or(0, ffi_disposition),
        cache_key_sha256,
        artifact_identity_sha256,
        width,
        height,
        diagnostic: snapshot.diagnostic,
    }
}

fn empty_ready_projection() -> (String, String, u32, u32) {
    (String::new(), String::new(), 0, 0)
}

const fn ffi_phase(phase: RawFoundationJobPhase) -> ffi::FfiRawFoundationJobPhase {
    match phase {
        RawFoundationJobPhase::Queued => ffi::FfiRawFoundationJobPhase::Queued,
        RawFoundationJobPhase::Planning => ffi::FfiRawFoundationJobPhase::Planning,
        RawFoundationJobPhase::Running => ffi::FfiRawFoundationJobPhase::Running,
        RawFoundationJobPhase::Ready => ffi::FfiRawFoundationJobPhase::Ready,
        RawFoundationJobPhase::Unavailable => ffi::FfiRawFoundationJobPhase::Unavailable,
        RawFoundationJobPhase::Cancelled => ffi::FfiRawFoundationJobPhase::Cancelled,
        RawFoundationJobPhase::Failed => ffi::FfiRawFoundationJobPhase::Failed,
    }
}

const fn ffi_disposition(disposition: RawFoundationMaterializationDisposition) -> u8 {
    match disposition {
        RawFoundationMaterializationDisposition::ReusedVerified => 1,
        RawFoundationMaterializationDisposition::Published => 2,
        RawFoundationMaterializationDisposition::ReusedConcurrent => 3,
    }
}

#[cfg(test)]
mod tests;
