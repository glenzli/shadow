//! Provider-facing photo access for the desktop process.
//!
//! This module is the narrow ownership boundary between ordinary public photo
//! decoding and the crash-isolated helper used by an installed private RAW
//! provider.  The desktop session consumes its previews or temporary raster
//! paths but never loads a private SDK itself.

use std::path::{Path, PathBuf};

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_bridge::{
    RawDevelopmentPlan, extract_best_photo_preview, inspect_photo, photo_provider_version,
    photo_supported_raster_extensions, raw_development_plan_identity, render_photo_reference_proxy,
};
use shadow_core::DecodeInspector;
use shadow_domain::{DecoderSnapshot, PreviewPayload, ProxyPayload};

use crate::isolated_proxy::{
    configured_helper_path, render_isolated_photo_reference_proxy,
    render_isolated_photo_reference_proxy_to_file,
};

#[derive(Debug, Clone)]
pub(crate) struct PhotoInspector {
    version: String,
    original_raster_extensions: Vec<String>,
    proxy_variant_key: String,
    isolated_proxy_runtime_cache: Option<PathBuf>,
}

// The generated-library proxy is deliberately a lower-bandwidth artifact than the warm editing
// preview. Keep its encoder request and persistent variant identity next to each other: a stale
// or misleading key would otherwise make the catalog serve the wrong cache entry indefinitely.
pub(crate) const PHOTO_GRID_PROXY_MAX_EDGE: u32 = 2_048;
pub(crate) const PHOTO_GRID_PROXY_JPEG_QUALITY: u8 = 88;
const ISOLATED_EDIT_JPEG_QUALITY: u8 = 96;

impl PhotoInspector {
    #[cfg(test)]
    pub(crate) fn new() -> AnyResult<Self> {
        Self::new_with_isolated_proxy_cache(None)
    }

    pub(crate) fn new_with_isolated_proxy_cache(
        runtime_cache_root: Option<PathBuf>,
    ) -> AnyResult<Self> {
        let raw_development_plan_identity =
            raw_development_plan_identity(RawDevelopmentPlan::preview())
                .context("build grid-proxy RAW-development cache identity")?;
        Ok(Self {
            version: photo_provider_version(),
            original_raster_extensions: photo_supported_raster_extensions(),
            // The source provider version identifies implementation releases; this exact plan
            // identity distinguishes two renders through the same provider with different
            // source-development intent or policy.
            proxy_variant_key: format!(
                "shadow-photo-router:grid-jpeg-2048-q88-444-v2;{raw_development_plan_identity}"
            ),
            isolated_proxy_runtime_cache: runtime_cache_root,
        })
    }
}

impl DecodeInspector for PhotoInspector {
    fn provider_id(&self) -> &'static str {
        "shadow-photo-router"
    }

    fn provider_version(&self) -> &str {
        &self.version
    }

    fn supported_original_raster_extensions(&self) -> Vec<String> {
        self.original_raster_extensions.clone()
    }

    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
        inspect_photo(path).map_err(|error| error.to_string())
    }

    fn extract_best_preview(&mut self, path: &Path) -> Result<Option<PreviewPayload>, String> {
        extract_best_photo_preview(path).map_err(|error| error.to_string())
    }

    fn render_proxy(&mut self, path: &Path) -> Result<Option<ProxyPayload>, String> {
        if let Some(runtime_cache_root) = &self.isolated_proxy_runtime_cache {
            let helper_path = configured_helper_path().ok_or_else(|| {
                "isolated RAW decode helper is unavailable; Shadow will not run a native decoder inside the desktop process".to_owned()
            })?;
            return render_isolated_photo_reference_proxy(
                &helper_path,
                runtime_cache_root,
                path,
                PHOTO_GRID_PROXY_MAX_EDGE,
                PHOTO_GRID_PROXY_JPEG_QUALITY,
            )
            .map(Some)
            .map_err(|error| error.to_string());
        }
        render_photo_reference_proxy(
            path,
            PHOTO_GRID_PROXY_MAX_EDGE,
            PHOTO_GRID_PROXY_JPEG_QUALITY,
        )
        .map(Some)
        .map_err(|error| error.to_string())
    }

    fn proxy_variant_key(&self) -> &str {
        &self.proxy_variant_key
    }
}

/// Develops a source through the crash-isolated helper and returns the short-lived JPEG path
/// that the public raster edit path can open.  The caller removes the path after preparation:
/// edit sessions retain decoded pixels, not an open file descriptor.
pub(crate) fn isolated_edit_raster(
    runtime_cache_root: &Path,
    source_path: &Path,
    max_edge: u32,
) -> AnyResult<PathBuf> {
    let helper_path = configured_helper_path().ok_or_else(|| {
        anyhow!(
            "isolated RAW decoder is unavailable; Shadow will not load a private decoder in the desktop process"
        )
    })?;
    let (_, temporary_raster) = render_isolated_photo_reference_proxy_to_file(
        &helper_path,
        runtime_cache_root,
        source_path,
        max_edge,
        ISOLATED_EDIT_JPEG_QUALITY,
    )
    .with_context(|| {
        format!(
            "develop {} through the isolated RAW decoder",
            source_path.display()
        )
    })?;
    Ok(temporary_raster)
}
