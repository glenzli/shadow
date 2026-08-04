//! Provider-facing photo access for the desktop process.
//!
//! This module is the narrow ownership boundary between ordinary public photo
//! decoding and the crash-isolated helper used by an installed private RAW
//! provider.  The desktop session consumes its previews or temporary raster
//! paths but never loads a private SDK itself.

mod grid_proxy_identity;

use std::path::{Path, PathBuf};

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_bridge::{
    RawDevelopmentPlan, extract_best_photo_preview, inspect_photo, photo_provider_version,
    photo_supported_raster_extensions, raw_development_plan_identity, render_photo_reference_proxy,
};
use shadow_core::DecodeInspector;
use shadow_domain::{DecoderSnapshot, PreviewPayload, ProxyPayload};

use self::grid_proxy_identity::grid_proxy_variant_key;
use crate::isolated_proxy::{
    ProviderHostInventory, configured_helper_path, isolated_helper_implementation_identity,
    render_isolated_photo_reference_proxy, render_isolated_photo_reference_proxy_to_file,
    snapshot_isolated_photo_decoder,
};

#[derive(Debug, Clone)]
pub struct PhotoInspector {
    version: String,
    original_raster_extensions: Vec<String>,
    proxy_variant_key: String,
    isolated_proxy_runtime_cache: Option<PathBuf>,
    isolated_helper_path: Option<PathBuf>,
}

// The generated-library proxy is deliberately a lower-bandwidth artifact than the warm editing
// preview. Keep its encoder request and persistent variant identity next to each other: a stale
// or misleading key would otherwise make the catalog serve the wrong cache entry indefinitely.
pub(crate) const PHOTO_GRID_PROXY_MAX_EDGE: u32 = 2_048;
pub(crate) const PHOTO_GRID_PROXY_JPEG_QUALITY: u8 = 90;
const ISOLATED_EDIT_JPEG_QUALITY: u8 = 96;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
enum InspectionRoute {
    Direct,
    IsolatedRaw,
}

impl PhotoInspector {
    #[cfg(test)]
    pub(crate) fn new() -> AnyResult<Self> {
        Self::new_with_isolated_proxy_cache(None)
    }

    pub(crate) fn new_with_isolated_proxy_cache(
        runtime_cache_root: Option<PathBuf>,
    ) -> AnyResult<Self> {
        let helper_path = runtime_cache_root
            .is_some()
            .then(configured_helper_path)
            .flatten();
        let raw_development_plan_identity =
            raw_development_plan_identity(RawDevelopmentPlan::preview())
                .context("build grid-proxy RAW-development cache identity")?;
        // A helper/private-provider replacement must invalidate the catalog's
        // inspection and generated-proxy version even though the outer public
        // router's ABI version did not change. The digest carries no private
        // path, only the configured helper graph identity.
        let isolated_implementation_identity = runtime_cache_root
            .is_some()
            .then(|| isolated_helper_implementation_identity(helper_path.as_deref()))
            .transpose()?;
        let version = match &isolated_implementation_identity {
            Some(identity) => format!(
                "{};isolated-helper-graph={identity}",
                photo_provider_version()
            ),
            None => photo_provider_version(),
        };
        let proxy_variant_key = grid_proxy_variant_key(
            PHOTO_GRID_PROXY_MAX_EDGE,
            PHOTO_GRID_PROXY_JPEG_QUALITY,
            &raw_development_plan_identity,
            isolated_implementation_identity.as_deref(),
        );
        Ok(Self {
            version,
            original_raster_extensions: photo_supported_raster_extensions(),
            // The source provider version identifies implementation releases; this exact plan
            // identity distinguishes two renders through the same provider with different
            // source-development intent or policy.
            proxy_variant_key,
            isolated_proxy_runtime_cache: runtime_cache_root,
            isolated_helper_path: helper_path,
        })
    }

    /// Builds the application-level photo inspector around a verified isolated Provider Host.
    ///
    /// The parent process never loads the private module. The child-provided router version is
    /// folded into both inspection and proxy cache identities, including discovery-mode plugins
    /// whose paths deliberately remain outside the Catalog.
    ///
    /// # Errors
    ///
    /// Returns an error for a public-only inventory or an unavailable helper identity.
    pub fn new_with_provider_host(
        runtime_cache_root: PathBuf,
        helper_path: PathBuf,
        inventory: &ProviderHostInventory,
    ) -> AnyResult<Self> {
        if !inventory.private_provider_available {
            anyhow::bail!("Provider Host inventory does not contain a private decoder");
        }
        let raw_development_plan_identity =
            raw_development_plan_identity(RawDevelopmentPlan::preview())
                .context("build grid-proxy RAW-development cache identity")?;
        let helper_identity = isolated_helper_implementation_identity(Some(&helper_path))?;
        let router_digest = blake3::hash(inventory.router_version.as_bytes())
            .to_hex()
            .to_string();
        let isolated_implementation_identity =
            format!("helper={helper_identity};router={router_digest}");
        let version = format!(
            "shadow-photo-provider-host-v1;isolated-helper-graph={isolated_implementation_identity}"
        );
        let proxy_variant_key = grid_proxy_variant_key(
            PHOTO_GRID_PROXY_MAX_EDGE,
            PHOTO_GRID_PROXY_JPEG_QUALITY,
            &raw_development_plan_identity,
            Some(&isolated_implementation_identity),
        );
        Ok(Self {
            version,
            original_raster_extensions: photo_supported_raster_extensions(),
            proxy_variant_key,
            isolated_proxy_runtime_cache: Some(runtime_cache_root),
            isolated_helper_path: Some(helper_path),
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
        match self.inspection_route(path) {
            InspectionRoute::Direct => inspect_photo(path).map_err(|error| error.to_string()),
            InspectionRoute::IsolatedRaw => {
                let runtime_cache_root = self
                    .isolated_proxy_runtime_cache
                    .as_deref()
                    .ok_or_else(|| "isolated RAW inspection cache is unavailable".to_owned())?;
                let helper_path = self.isolated_helper_path.as_deref().ok_or_else(|| {
                    "isolated RAW decode helper is unavailable; Shadow will not run a native decoder inside the desktop process".to_owned()
                })?;
                snapshot_isolated_photo_decoder(helper_path, runtime_cache_root, path)
                    .map(|snapshot| {
                        // The child may route through a private provider whose
                        // identity the desktop intentionally cannot load. The
                        // catalog's stable inspector contract therefore remains
                        // the public outer router; helper identity stays in the
                        // local isolated snapshot cache only.
                        snapshot.into_catalog_snapshot(self.provider_id(), self.provider_version())
                    })
                    .map_err(|error| error.to_string())
            }
        }
    }

    fn extract_best_preview(&mut self, path: &Path) -> Result<Option<PreviewPayload>, String> {
        match self.inspection_route(path) {
            // The catalog worker will immediately ask render_proxy after this
            // legitimate None. Never reopen a helper-enabled RAW merely to
            // decode embedded preview bytes in the desktop process.
            InspectionRoute::IsolatedRaw => Ok(None),
            InspectionRoute::Direct => {
                extract_best_photo_preview(path).map_err(|error| error.to_string())
            }
        }
    }

    fn render_proxy(&mut self, path: &Path) -> Result<Option<ProxyPayload>, String> {
        if let Some(runtime_cache_root) = &self.isolated_proxy_runtime_cache {
            let helper_path = self.isolated_helper_path.as_deref().ok_or_else(|| {
                "isolated RAW decode helper is unavailable; Shadow will not run a native decoder inside the desktop process".to_owned()
            })?;
            return render_isolated_photo_reference_proxy(
                helper_path,
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

impl PhotoInspector {
    fn is_supported_original_raster(&self, path: &Path) -> bool {
        let Some(extension) = path.extension().and_then(|extension| extension.to_str()) else {
            return false;
        };
        self.original_raster_extensions
            .iter()
            .any(|supported| supported.eq_ignore_ascii_case(extension))
    }

    fn inspection_route(&self, path: &Path) -> InspectionRoute {
        if self.isolated_proxy_runtime_cache.is_some() && !self.is_supported_original_raster(path) {
            InspectionRoute::IsolatedRaw
        } else {
            InspectionRoute::Direct
        }
    }
}

#[cfg(test)]
mod tests;

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
