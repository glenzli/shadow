//! Desktop filesystem and explicit execution-route layout for `RawNIND`.

use std::{
    ffi::{OsStr, OsString},
    path::{Path, PathBuf},
};

use thiserror::Error;

const PROVIDER_OVERRIDE: &str = "SHADOW_RAWNIND_PROVIDER_PATH";
const PACKAGE_OVERRIDE: &str = "SHADOW_RAWNIND_PACKAGE_PATH";
const GRAPH_OVERRIDE: &str = "SHADOW_RAWNIND_BAYER_GRAPH_PATH";
const MANIFEST_OVERRIDE: &str = "SHADOW_RAWNIND_MANIFEST_PATH";
const EXECUTION_ROUTE_OVERRIDE: &str = "SHADOW_RAW_FOUNDATION_EXECUTION";
const INFER_BASE_URL_OVERRIDE: &str = "SHADOW_INFER_BASE_URL";
const INFER_CREDENTIAL_OVERRIDE: &str = "SHADOW_INFER_CREDENTIAL_FILE";
const PROVIDER_EXECUTABLE_NAME: &str = "shadow-rawnind-foundation-provider";
const PROVIDER_MANIFEST_NAME: &str = "shadow-rawnind-foundation-model-manifest.json";

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) enum RawFoundationExecutionRoute {
    LegacySidecar,
    InferRuntime,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationRuntimePaths {
    pub(crate) provider_executable: PathBuf,
    pub(crate) model_package: PathBuf,
    pub(crate) model_graph: PathBuf,
    pub(crate) manifest_path: PathBuf,
    pub(crate) foundation_store_root: PathBuf,
    pub(crate) raw_frame_staging_root: PathBuf,
    pub(crate) execution_route: RawFoundationExecutionRoute,
    pub(crate) infer_base_url_override: Option<String>,
    pub(crate) infer_credential_file: PathBuf,
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
        let execution_route = match environment(EXECUTION_ROUTE_OVERRIDE)
            .filter(|value| !value.is_empty())
            .as_deref()
        {
            None => RawFoundationExecutionRoute::LegacySidecar,
            Some(value) if value == OsStr::new("legacy-sidecar") => {
                RawFoundationExecutionRoute::LegacySidecar
            }
            Some(value) if value == OsStr::new("infer-runtime") => {
                RawFoundationExecutionRoute::InferRuntime
            }
            Some(_) => return Err(RawFoundationRuntimePathError::InvalidExecutionRoute),
        };
        let infer_base_url_override = environment(INFER_BASE_URL_OVERRIDE)
            .filter(|value| !value.is_empty())
            .map(OsString::into_string)
            .transpose()
            .map_err(|_| RawFoundationRuntimePathError::InvalidInferBaseUrl)?;

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
            raw_frame_staging_root: cache_root.join("ai").join("raw-frame-staging"),
            execution_route,
            infer_base_url_override,
            infer_credential_file: override_path(&environment, INFER_CREDENTIAL_OVERRIDE)
                .unwrap_or_else(|| {
                    application_data_root
                        .join("credentials")
                        .join("infer-runtime-shadow.token")
                }),
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
    #[error("SHADOW_RAW_FOUNDATION_EXECUTION must be legacy-sidecar or infer-runtime")]
    InvalidExecutionRoute,
    #[error("SHADOW_INFER_BASE_URL is not valid UTF-8")]
    InvalidInferBaseUrl,
}

#[cfg(test)]
mod tests;
