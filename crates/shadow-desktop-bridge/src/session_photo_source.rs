//! Catalog source admission, native decode quarantine, and optics discovery.

use std::path::{Path, PathBuf};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_bridge::{
    photo_supported_raster_extensions, query_optics_profiles_from_metadata,
    query_photo_optics_profiles,
};
use shadow_catalog::ReviewItemRecord;
use shadow_domain::PhotoId;

use super::{
    DesktopSession, ffi,
    isolated_proxy::{
        NativeDecodeAdmission, configured_helper_path,
        native_decode_admission_after_isolated_stages, snapshot_isolated_photo_metadata,
    },
};

#[cfg(unix)]
pub(crate) fn catalog_native_path(source: &ReviewItemRecord) -> AnyResult<PathBuf> {
    use std::{ffi::OsString, os::unix::ffi::OsStringExt};

    match source.location.platform {
        shadow_domain::Platform::MacOs | shadow_domain::Platform::OtherUnix => Ok(PathBuf::from(
            OsString::from_vec(source.location.native_path.clone()),
        )),
        shadow_domain::Platform::Windows => {
            bail!("a Windows-native source path cannot be decoded by the Mac desktop service")
        }
    }
}

#[cfg(not(unix))]
pub(crate) fn catalog_native_path(_source: &ReviewItemRecord) -> AnyResult<PathBuf> {
    bail!("the first desktop edit service currently decodes native paths only on macOS")
}

/// A child crash or timeout is durable negative evidence for this exact source
/// and helper revision. Preserve warm sessions, but do not reopen the native
/// source in the desktop process until that evidence no longer applies.
pub(crate) fn ensure_native_decode_is_admitted(
    runtime_cache_root: &Path,
    native_path: &Path,
) -> AnyResult<()> {
    let Some(helper_path) = configured_helper_path() else {
        return Ok(());
    };
    let admission = native_decode_admission_after_isolated_stages(
        runtime_cache_root,
        native_path,
        &helper_path,
    )?;
    reject_quarantined_native_decode(admission, native_path)
}

pub(crate) fn reject_quarantined_native_decode(
    admission: NativeDecodeAdmission,
    native_path: &Path,
) -> AnyResult<()> {
    match admission {
        NativeDecodeAdmission::NotQuarantined => Ok(()),
        NativeDecodeAdmission::Quarantined { observation } => bail!(
            "native edit decode remains disabled for {}: {}. This photo is temporarily preview-only until its source or decoder helper changes",
            native_path.display(),
            observation.diagnostic_label(),
        ),
    }
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum MissingCatalogOpticsRoute<'a> {
    DirectNative,
    IsolatedMetadata(&'a Path),
}

// Keep this source-shape decision alongside the desktop fallback that consumes it. It mirrors the
// catalog inspector's public-raster exception: a configured helper isolates non-raster sources,
// while ordinary JPEG/HEIF input retains its established direct metadata route.
pub(crate) fn missing_catalog_optics_route<'a>(
    native_path: &Path,
    helper_path: Option<&'a Path>,
) -> MissingCatalogOpticsRoute<'a> {
    let Some(helper_path) = helper_path else {
        return MissingCatalogOpticsRoute::DirectNative;
    };
    let is_supported_raster = native_path
        .extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| {
            photo_supported_raster_extensions()
                .iter()
                .any(|supported| supported.eq_ignore_ascii_case(extension))
        });
    if is_supported_raster {
        MissingCatalogOpticsRoute::DirectNative
    } else {
        MissingCatalogOpticsRoute::IsolatedMetadata(helper_path)
    }
}

pub(crate) fn query_missing_catalog_optics_profiles(
    native_path: &Path,
    runtime_cache_root: &Path,
    helper_path: Option<&Path>,
) -> AnyResult<Vec<shadow_bridge::OpticsProfileCandidate>> {
    match missing_catalog_optics_route(native_path, helper_path) {
        MissingCatalogOpticsRoute::DirectNative => Ok(query_photo_optics_profiles(native_path)?),
        MissingCatalogOpticsRoute::IsolatedMetadata(helper_path) => {
            let snapshot =
                snapshot_isolated_photo_metadata(helper_path, runtime_cache_root, native_path)
                    .context(
                        "optics profile metadata is unavailable from the isolated RAW helper",
                    )?;
            Ok(query_optics_profiles_from_metadata(&snapshot.metadata))
        }
    }
}

impl DesktopSession {
    pub(crate) fn optics_profile_candidates(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<Vec<ffi::FfiOpticsProfileCandidate>> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        // Profile discovery is a metadata operation, not a RAW-pixel operation. Prefer the
        // Catalog snapshot so a proprietary compression can still match camera/lens EXIF even
        // when the active open decoder cannot unpack it. The file path is only a compatibility
        // fallback for photos imported before metadata snapshots existed.
        let candidates = if let Some(metadata) = self
            .catalog
            .review_source(photo_id)?
            .and_then(|raw| raw.metadata)
        {
            query_optics_profiles_from_metadata(&metadata)
        } else {
            query_missing_catalog_optics_profiles(
                &catalog_native_path(&source)?,
                &self.cache_root,
                configured_helper_path().as_deref(),
            )?
        };
        Ok(candidates
            .into_iter()
            .map(|candidate| ffi::FfiOpticsProfileCandidate {
                camera_maker: candidate.camera_maker,
                camera_model: candidate.camera_model,
                lens_maker: candidate.lens_maker,
                lens_model: candidate.lens_model,
            })
            .collect())
    }

    pub(crate) fn validated_photo_source(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<(PhotoId, ReviewItemRecord)> {
        let photo_id: PhotoId = photo_id
            .parse()
            .with_context(|| format!("parse photo id {photo_id}"))?;
        let source = self
            .catalog
            .photo_source(photo_id)?
            .ok_or_else(|| anyhow!("photo {photo_id} has no online original photo source"))?;
        if source.location.display_path != source_path {
            bail!(
                "source path does not belong to photo {photo_id}: expected {}, received {source_path}",
                source.location.display_path
            );
        }
        Ok((photo_id, source))
    }
}
