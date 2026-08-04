//! Embedded-preview-first decoder routing for the managed Library server.

use std::{
    env,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use shadow_bridge::{
    extract_best_libraw_preview, inspect_libraw, libraw_provider_version,
    render_libraw_reference_proxy,
};
use shadow_core::DecodeInspector;
use shadow_domain::{DecoderSnapshot, PreviewPayload, ProxyPayload};

use crate::{PhotoInspector, inspect_provider_host};

const HELPER_PATH_ENVIRONMENT: &str = "SHADOW_DECODE_HELPER_PATH";
const HELPER_EXECUTABLE_NAME: &str = "shadow-image-decode-helper";

#[derive(Debug, Clone)]
struct LibRawInspector {
    version: String,
}

impl LibRawInspector {
    fn new() -> Self {
        Self {
            version: libraw_provider_version(),
        }
    }
}

impl DecodeInspector for LibRawInspector {
    fn provider_id(&self) -> &'static str {
        "libraw"
    }

    fn provider_version(&self) -> &str {
        &self.version
    }

    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
        inspect_libraw(path).map_err(|error| error.to_string())
    }

    fn extract_best_preview(&mut self, path: &Path) -> Result<Option<PreviewPayload>, String> {
        extract_best_libraw_preview(path).map_err(|error| error.to_string())
    }

    fn render_proxy(&mut self, path: &Path) -> Result<Option<ProxyPayload>, String> {
        render_libraw_reference_proxy(path, 2_048, 88)
            .map(Some)
            .map_err(|error| error.to_string())
    }

    fn proxy_variant_key(&self) -> &'static str {
        "libraw:grid-jpeg-2048-q88-444-v1"
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
enum PreviewRoute {
    Public,
    ProviderHost,
}

#[derive(Debug, Clone)]
pub(super) struct LibraryServerPreviewInspector {
    public: LibRawInspector,
    provider_host: Option<PhotoInspector>,
    provider_version: String,
    proxy_variant_key: String,
    active_source: Option<(PathBuf, PreviewRoute)>,
}

impl LibraryServerPreviewInspector {
    fn public_only() -> Self {
        let public = LibRawInspector::new();
        Self {
            provider_version: format!(
                "remote-library-preview-v1;public={}",
                public.provider_version()
            ),
            proxy_variant_key: format!(
                "remote-library-preview-v1;public={}",
                public.proxy_variant_key()
            ),
            public,
            provider_host: None,
            active_source: None,
        }
    }

    fn with_provider_host(provider_host: PhotoInspector) -> Self {
        let public = LibRawInspector::new();
        Self {
            provider_version: format!(
                "remote-library-preview-v1;public={};provider-host={}",
                public.provider_version(),
                provider_host.provider_version()
            ),
            proxy_variant_key: format!(
                "remote-library-preview-v1;public={};provider-host={}",
                public.proxy_variant_key(),
                provider_host.proxy_variant_key()
            ),
            public,
            provider_host: Some(provider_host),
            active_source: None,
        }
    }

    fn normalize_snapshot(&self, mut snapshot: DecoderSnapshot) -> DecoderSnapshot {
        self.provider_id().clone_into(&mut snapshot.provider.id);
        snapshot.provider.version.clone_from(&self.provider_version);
        snapshot
    }

    fn active_route(&self, path: &Path) -> Result<PreviewRoute, String> {
        self.active_source
            .as_ref()
            .filter(|(active_path, _)| active_path == path)
            .map(|(_, route)| *route)
            .ok_or_else(|| {
                "remote Library preview route was not established by the current inspection"
                    .to_owned()
            })
    }
}

impl DecodeInspector for LibraryServerPreviewInspector {
    fn provider_id(&self) -> &'static str {
        "shadow-remote-library-preview-router"
    }

    fn provider_version(&self) -> &str {
        &self.provider_version
    }

    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
        self.active_source = None;
        let public = self.public.inspect(path);
        if let Ok(snapshot) = &public
            && (snapshot.capabilities.embedded_previews.is_available()
                || snapshot.capabilities.reference_rgb.is_available())
        {
            self.active_source = Some((path.to_path_buf(), PreviewRoute::Public));
            return Ok(self.normalize_snapshot(snapshot.clone()));
        }

        let Some(provider_host) = self.provider_host.as_mut() else {
            return public.map(|snapshot| {
                self.active_source = Some((path.to_path_buf(), PreviewRoute::Public));
                self.normalize_snapshot(snapshot)
            });
        };
        match provider_host.inspect(path) {
            Ok(snapshot) => {
                self.active_source = Some((path.to_path_buf(), PreviewRoute::ProviderHost));
                Ok(self.normalize_snapshot(snapshot))
            }
            Err(host_error) => Err(match public {
                Ok(_) => format!(
                    "private decoder Provider Host could not prepare this RAW: {host_error}"
                ),
                Err(public_error) => format!(
                    "public decoder failed ({public_error}); private decoder Provider Host failed ({host_error})"
                ),
            }),
        }
    }

    fn extract_best_preview(&mut self, path: &Path) -> Result<Option<PreviewPayload>, String> {
        match self.active_route(path)? {
            PreviewRoute::Public => self.public.extract_best_preview(path).or(Ok(None)),
            PreviewRoute::ProviderHost => Ok(None),
        }
    }

    fn render_proxy(&mut self, path: &Path) -> Result<Option<ProxyPayload>, String> {
        match self.active_route(path)? {
            PreviewRoute::ProviderHost => self
                .provider_host
                .as_mut()
                .expect("Provider Host route requires an admitted inspector")
                .render_proxy(path),
            PreviewRoute::Public => match self.public.render_proxy(path) {
                Ok(Some(proxy)) => Ok(Some(proxy)),
                Ok(None) | Err(_) => self
                    .provider_host
                    .as_mut()
                    .map_or(Ok(None), |provider_host| provider_host.render_proxy(path)),
            },
        }
    }

    fn proxy_variant_key(&self) -> &str {
        &self.proxy_variant_key
    }
}

#[derive(Debug)]
pub(super) struct LibraryServerPreviewRuntime {
    inspector: LibraryServerPreviewInspector,
    mode: ProviderHostMode,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
enum ProviderHostMode {
    Absent,
    PublicOnly,
    Private,
}

impl LibraryServerPreviewRuntime {
    pub(super) fn discover(cache_root: &Path) -> Result<Self> {
        let Some(helper_path) = configured_provider_host_path()? else {
            return Ok(Self {
                inspector: LibraryServerPreviewInspector::public_only(),
                mode: ProviderHostMode::Absent,
            });
        };
        let inventory = inspect_provider_host(&helper_path).with_context(|| {
            format!(
                "verify private decoder Provider Host {}",
                helper_path.display()
            )
        })?;
        if !inventory.private_provider_available {
            return Ok(Self {
                inspector: LibraryServerPreviewInspector::public_only(),
                mode: ProviderHostMode::PublicOnly,
            });
        }
        let runtime_cache = cache_root.join("provider-host-runtime");
        let inspector =
            PhotoInspector::new_with_provider_host(runtime_cache, helper_path, &inventory)?;
        Ok(Self {
            inspector: LibraryServerPreviewInspector::with_provider_host(inspector),
            mode: ProviderHostMode::Private,
        })
    }

    pub(super) fn private_provider_available(&self) -> bool {
        self.mode == ProviderHostMode::Private
    }

    pub(super) const fn mode_label(&self) -> &'static str {
        match self.mode {
            ProviderHostMode::Absent => "absent",
            ProviderHostMode::PublicOnly => "public-only",
            ProviderHostMode::Private => "private",
        }
    }

    pub(super) fn into_inspector(self) -> LibraryServerPreviewInspector {
        self.inspector
    }
}

fn configured_provider_host_path() -> Result<Option<PathBuf>> {
    if let Some(configured) = env::var_os(HELPER_PATH_ENVIRONMENT) {
        let path = PathBuf::from(configured);
        if !path.is_file() {
            bail!(
                "configured private decoder Provider Host is not a file: {}",
                path.display()
            );
        }
        return Ok(Some(path));
    }
    let executable = env::current_exe().context("locate remote Library server executable")?;
    let sibling = executable
        .parent()
        .map(|parent| parent.join(HELPER_EXECUTABLE_NAME));
    Ok(sibling.filter(|path| path.is_file()))
}

#[cfg(test)]
mod tests;
