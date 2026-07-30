//! Desktop filesystem layout for the side-loaded `RawNIND` provider.

use std::{
    ffi::{OsStr, OsString},
    path::{Path, PathBuf},
};

use thiserror::Error;

const PROVIDER_OVERRIDE: &str = "SHADOW_RAWNIND_PROVIDER_PATH";
const PACKAGE_OVERRIDE: &str = "SHADOW_RAWNIND_PACKAGE_PATH";
const GRAPH_OVERRIDE: &str = "SHADOW_RAWNIND_BAYER_GRAPH_PATH";
const MANIFEST_OVERRIDE: &str = "SHADOW_RAWNIND_MANIFEST_PATH";
const PROVIDER_EXECUTABLE_NAME: &str = "shadow-rawnind-foundation-provider";
const PROVIDER_MANIFEST_NAME: &str = "shadow-rawnind-foundation-model-manifest.json";

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationRuntimePaths {
    pub(crate) provider_executable: PathBuf,
    pub(crate) model_package: PathBuf,
    pub(crate) model_graph: PathBuf,
    pub(crate) manifest_path: PathBuf,
    pub(crate) foundation_store_root: PathBuf,
}

impl RawFoundationRuntimePaths {
    pub(crate) fn discover(
        desktop_executable: &Path,
        cache_root: &Path,
    ) -> Result<Self, RawFoundationRuntimePathError> {
        Self::discover_with(desktop_executable, cache_root, |key| std::env::var_os(key))
    }

    fn discover_with(
        desktop_executable: &Path,
        cache_root: &Path,
        environment: impl Fn(&str) -> Option<OsString>,
    ) -> Result<Self, RawFoundationRuntimePathError> {
        let executable_directory = desktop_executable
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .ok_or(RawFoundationRuntimePathError::ExecutableDirectory)?;
        let application_data_root = cache_root
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .ok_or(RawFoundationRuntimePathError::ApplicationDataDirectory)?;
        let model_root = application_data_root
            .join("models")
            .join("rawnind-public-bayer-release-5.6.0");
        let provider_root = packaged_provider_root(executable_directory);

        Ok(Self {
            provider_executable: override_path(&environment, PROVIDER_OVERRIDE)
                .unwrap_or_else(|| provider_root.join(PROVIDER_EXECUTABLE_NAME)),
            model_package: override_path(&environment, PACKAGE_OVERRIDE)
                .unwrap_or_else(|| model_root.join("rawdenoise-nind.dtmodel")),
            model_graph: override_path(&environment, GRAPH_OVERRIDE)
                .unwrap_or_else(|| model_root.join("model_bayer.onnx")),
            manifest_path: override_path(&environment, MANIFEST_OVERRIDE)
                .unwrap_or_else(|| provider_root.join(PROVIDER_MANIFEST_NAME)),
            foundation_store_root: cache_root.join("ai").join("raw-foundations"),
        })
    }
}

fn packaged_provider_root(executable_directory: &Path) -> PathBuf {
    if executable_directory.file_name() == Some(OsStr::new("MacOS"))
        && executable_directory.parent().and_then(Path::file_name) == Some(OsStr::new("Contents"))
    {
        return executable_directory
            .parent()
            .expect("checked Contents parent")
            .join("Helpers")
            .join("RawNIND");
    }
    executable_directory.to_path_buf()
}

fn override_path(environment: &impl Fn(&str) -> Option<OsString>, key: &str) -> Option<PathBuf> {
    environment(key)
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
}

#[derive(Debug, Error)]
pub(crate) enum RawFoundationRuntimePathError {
    #[error("desktop executable has no containing directory")]
    ExecutableDirectory,
    #[error("desktop cache root has no application-data parent")]
    ApplicationDataDirectory,
}

#[cfg(test)]
mod tests;
