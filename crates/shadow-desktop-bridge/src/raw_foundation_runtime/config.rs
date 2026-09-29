//! Desktop-owned cache and Infer Runtime consumer configuration for RawNIND.

use std::{
    ffi::OsString,
    path::{Path, PathBuf},
};

use thiserror::Error;

const INFER_BASE_URL_OVERRIDE: &str = "SHADOW_INFER_BASE_URL";
const INFER_CREDENTIAL_OVERRIDE: &str = "SHADOW_INFER_CREDENTIAL_FILE";

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationRuntimePaths {
    pub(crate) foundation_store_root: PathBuf,
    pub(crate) raw_frame_staging_root: PathBuf,
    pub(crate) infer_base_url_override: Option<String>,
    pub(crate) infer_credential_file: PathBuf,
}

impl RawFoundationRuntimePaths {
    pub(crate) fn discover(cache_root: &Path) -> Result<Self, RawFoundationRuntimePathError> {
        Self::discover_with(cache_root, |key| std::env::var_os(key))
    }

    fn discover_with(
        cache_root: &Path,
        environment: impl Fn(&str) -> Option<OsString>,
    ) -> Result<Self, RawFoundationRuntimePathError> {
        let application_data_root = cache_root
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .ok_or(RawFoundationRuntimePathError::ApplicationDataDirectory)?;
        let infer_base_url_override = environment(INFER_BASE_URL_OVERRIDE)
            .filter(|value| !value.is_empty())
            .map(OsString::into_string)
            .transpose()
            .map_err(|_| RawFoundationRuntimePathError::InvalidInferBaseUrl)?;

        Ok(Self {
            foundation_store_root: cache_root.join("ai").join("raw-foundations"),
            raw_frame_staging_root: cache_root.join("ai").join("raw-frame-staging"),
            infer_base_url_override,
            infer_credential_file: override_path(&environment, INFER_CREDENTIAL_OVERRIDE)
                .unwrap_or_else(|| {
                    crate::infer_runtime_credentials::default_credential_file(application_data_root)
                }),
        })
    }
}

fn override_path(environment: &impl Fn(&str) -> Option<OsString>, key: &str) -> Option<PathBuf> {
    environment(key)
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
}

#[derive(Debug, Error)]
pub(crate) enum RawFoundationRuntimePathError {
    #[error("desktop cache root has no application-data parent")]
    ApplicationDataDirectory,
    #[error("SHADOW_INFER_BASE_URL is not valid UTF-8")]
    InvalidInferBaseUrl,
}

#[cfg(test)]
mod tests;
