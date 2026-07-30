//! Verified handoff from the rebuildable `.shadowrawf` cache into edit rendering.
//!
//! A ready job is only a session-local locator. This owner rechecks the exact
//! Catalog source, portable descriptor, complete artifact, model provenance,
//! and pixel buffer immediately before the synchronous Rust-to-C++ transfer.

use std::path::Path;

use anyhow::{Context, Result as AnyResult, bail, ensure};
use shadow_ai::{CancellationToken, RawFoundationArtifact};
use shadow_bridge::{
    RAW_FOUNDATION_IMPLEMENTATION_REVISION, RawFoundationArtifactIdentity, VerifiedRawFoundation,
};
use shadow_cache::{FoundationArtifactReader, FoundationArtifactVerification, sha256_file};
use shadow_catalog::RepresentationFingerprint;
use shadow_core::fingerprint_source;
use shadow_domain::{ImageDimensions, RawFoundationDenoise, RawFoundationDenoiseModel};

use crate::{
    raw_foundation_runtime::{RawFoundationReady, RawFoundationRuntime},
    raw_foundation_service::RawFoundationService,
};

/// Complete cache discriminator for one verified RAW-foundation render input.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationRenderIdentity {
    source_sha256: String,
    artifact_file_sha256: String,
    cache_key_sha256: String,
    artifact_identity_sha256: String,
    model_package_sha256: String,
    model_graph_sha256: String,
    implementation_revision: String,
}

/// Recipe-selected, session-ready locator with a prevalidated cache identity.
#[derive(Debug, Clone)]
pub(crate) struct RawFoundationRenderSelection {
    ready: RawFoundationReady,
    model: RawFoundationDenoiseModel,
    pub(crate) identity: RawFoundationRenderIdentity,
}

impl RawFoundationRenderIdentity {
    pub(crate) fn from_ready(
        ready: &RawFoundationReady,
        model: RawFoundationDenoiseModel,
    ) -> AnyResult<Self> {
        ready
            .descriptor
            .validate()
            .context("validate ready RAW foundation descriptor")?;
        let descriptor = &ready.descriptor;
        let provenance = descriptor.provenance();
        ensure!(
            provenance.model_package_sha256() == model.package_sha256()
                && provenance.model_graph_sha256() == model.graph_sha256()
                && provenance.implementation_revision() == model.implementation_revision()
                && provenance.implementation_revision() == RAW_FOUNDATION_IMPLEMENTATION_REVISION,
            "ready RAW foundation does not implement the Recipe-selected model"
        );
        Ok(Self {
            source_sha256: descriptor.source().source_file_sha256().to_owned(),
            artifact_file_sha256: descriptor.artifact().content_hash().to_owned(),
            cache_key_sha256: provenance.cache_key_sha256().to_owned(),
            artifact_identity_sha256: provenance.artifact_identity_sha256().to_owned(),
            model_package_sha256: provenance.model_package_sha256().to_owned(),
            model_graph_sha256: provenance.model_graph_sha256().to_owned(),
            implementation_revision: provenance.implementation_revision().to_owned(),
        })
    }
}

/// Resolves enabled Recipe intent to one exact session-ready artifact.
///
/// Disabled intent is a true bypass. Enabled intent without a verified result
/// fails closed instead of silently rendering the ordinary RAW pipeline.
pub(crate) fn raw_foundation_ready_for_render(
    service: &RawFoundationService,
    runtime: &RawFoundationRuntime,
    source_path: &Path,
    source: RepresentationFingerprint,
    denoise: RawFoundationDenoise,
) -> AnyResult<Option<RawFoundationRenderSelection>> {
    if !denoise.is_enabled() {
        return Ok(None);
    }
    let ready = service.resolve_ready_for_source(
        runtime,
        source_path,
        source,
        &CancellationToken::default(),
    )?;
    select_ready_raw_foundation(ready, denoise)
}

fn select_ready_raw_foundation(
    ready: Option<RawFoundationReady>,
    denoise: RawFoundationDenoise,
) -> AnyResult<Option<RawFoundationRenderSelection>> {
    if !denoise.is_enabled() {
        return Ok(None);
    }
    let ready = ready.ok_or_else(|| {
        anyhow::anyhow!(
            "AI RAW denoise is enabled, but no verified foundation is cached for this source"
        )
    })?;
    let model = denoise.model();
    let identity = RawFoundationRenderIdentity::from_ready(&ready, model)?;
    Ok(Some(RawFoundationRenderSelection {
        ready,
        model,
        identity,
    }))
}

/// One fully verified, owned transfer plus its cache discriminator.
#[derive(Debug)]
pub(crate) struct LoadedRawFoundation {
    pub(crate) foundation: VerifiedRawFoundation,
}

/// Loads the exact session-ready artifact for an unchanged Catalog source.
///
/// The large pixel allocation exists only for this cold preparation call.
/// C++ borrows it synchronously and the returned native session owns its
/// prepared scene-linear source.
pub(crate) fn load_raw_foundation_for_render(
    selection: &RawFoundationRenderSelection,
    expected_source_path: &Path,
    expected_source: RepresentationFingerprint,
) -> AnyResult<LoadedRawFoundation> {
    let ready = &selection.ready;
    ensure!(
        ready.source_path == expected_source_path,
        "ready RAW foundation belongs to a different source path"
    );
    ensure!(
        ready.source == expected_source,
        "ready RAW foundation belongs to a different source revision"
    );
    let identity = RawFoundationRenderIdentity::from_ready(ready, selection.model)?;
    ensure!(
        identity == selection.identity,
        "ready RAW foundation identity changed after render selection"
    );
    let observed_before =
        fingerprint_source(expected_source_path).context("inventory RAW foundation source")?;
    ensure!(
        observed_before == expected_source,
        "RAW foundation source changed since Catalog registration"
    );
    ensure!(
        observed_before.byte_len == ready.descriptor.source().source_size_bytes(),
        "RAW foundation descriptor source size differs from the Catalog source"
    );
    let source_sha256 =
        sha256_file(expected_source_path).context("hash RAW foundation source before rendering")?;
    ensure!(
        source_sha256 == identity.source_sha256,
        "RAW foundation was generated from different source bytes"
    );

    let mut reader = FoundationArtifactReader::open(&ready.path)
        .with_context(|| format!("verify RAW foundation artifact {}", ready.path.display()))?;
    ensure_descriptor_matches(reader.verification(), &ready.descriptor)?;
    let verification = reader.verification().clone();
    let samples = reader
        .read_interleaved_rows(0, verification.height)
        .context("read verified RAW foundation pixels")?;
    let observed_after =
        fingerprint_source(expected_source_path).context("re-inventory RAW foundation source")?;
    if observed_after != expected_source {
        bail!("RAW foundation source changed while its render input was prepared");
    }

    let bridge_identity = RawFoundationArtifactIdentity::from_verified_digests(
        verification.source_sha256,
        verification.file_sha256,
        verification.cache_key_sha256,
    )
    .context("build native RAW foundation identity")?;
    let foundation = VerifiedRawFoundation::from_verified_interleaved_camera_rgb(
        ImageDimensions {
            width: verification.width,
            height: verification.height,
        },
        verification.force_rggb_crop_sensor[0],
        verification.force_rggb_crop_sensor[1],
        bridge_identity,
        samples,
    )
    .context("build verified native RAW foundation transfer")?;
    Ok(LoadedRawFoundation { foundation })
}

fn ensure_descriptor_matches(
    verification: &FoundationArtifactVerification,
    descriptor: &RawFoundationArtifact,
) -> AnyResult<()> {
    let extent = descriptor.raster_extent();
    let source = descriptor.source();
    let provenance = descriptor.provenance();
    ensure!(
        (verification.width, verification.height) == (extent.width, extent.height),
        "RAW foundation artifact dimensions differ from its ready descriptor"
    );
    ensure!(
        verification.file_sha256 == descriptor.artifact().content_hash()
            && verification.file_bytes == descriptor.artifact().byte_len(),
        "RAW foundation artifact bytes differ from its ready descriptor"
    );
    ensure!(
        verification.source_sha256 == source.source_file_sha256()
            && verification.source_size_bytes == source.source_size_bytes()
            && verification.source_pixel_contract_sha256 == source.source_pixel_contract_sha256(),
        "RAW foundation source provenance differs from its ready descriptor"
    );
    ensure!(
        verification.cache_key_sha256 == provenance.cache_key_sha256()
            && verification.artifact_identity_sha256 == provenance.artifact_identity_sha256()
            && verification.model_package_sha256 == provenance.model_package_sha256()
            && verification.model_graph_sha256 == provenance.model_graph_sha256()
            && verification.implementation_revision == provenance.implementation_revision(),
        "RAW foundation model or cache provenance differs from its ready descriptor"
    );
    Ok(())
}

#[cfg(test)]
mod tests;
