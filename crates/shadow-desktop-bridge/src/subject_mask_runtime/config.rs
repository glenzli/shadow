//! Desktop filesystem layout for Infer Runtime subject-mask consumption.

use std::{
    ffi::OsString,
    path::{Path, PathBuf},
};

use thiserror::Error;

const INFER_BASE_URL_OVERRIDE: &str = "SHADOW_INFER_BASE_URL";
const INFER_CREDENTIAL_OVERRIDE: &str = "SHADOW_INFER_CREDENTIAL_FILE";

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct SubjectMaskRuntimePaths {
    pub(crate) scratch_root: PathBuf,
    /// Durable generated objects are deliberately a sibling of `cache/`.
    /// Ordinary preview-cache removal must never invalidate a persisted Recipe.
    pub(crate) derived_raster_store_root: PathBuf,
    /// Explicit development override only; normal operation uses discovery.
    pub(crate) infer_base_url_override: Option<String>,
    pub(crate) infer_credential_file: PathBuf,
}

impl SubjectMaskRuntimePaths {
    pub(crate) fn discover(
        desktop_executable: &Path,
        cache_root: &Path,
    ) -> Result<Self, SubjectMaskRuntimePathError> {
        Self::discover_with(desktop_executable, cache_root, |key| std::env::var_os(key))
    }

    fn discover_with(
        desktop_executable: &Path,
        cache_root: &Path,
        environment: impl Fn(&str) -> Option<OsString>,
    ) -> Result<Self, SubjectMaskRuntimePathError> {
        let _ = desktop_executable
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .ok_or(SubjectMaskRuntimePathError::ExecutableDirectory)?;
        let application_data_root = cache_root
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .ok_or(SubjectMaskRuntimePathError::ApplicationDataDirectory)?;

        Ok(Self {
            scratch_root: cache_root.join("ai").join("subject-mask"),
            derived_raster_store_root: application_data_root.join("derived-rasters"),
            infer_base_url_override: environment(INFER_BASE_URL_OVERRIDE)
                .filter(|value| !value.is_empty())
                .map(OsString::into_string)
                .transpose()
                .map_err(|_| SubjectMaskRuntimePathError::InvalidInferBaseUrl)?,
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
pub(crate) enum SubjectMaskRuntimePathError {
    #[error("desktop executable has no containing directory")]
    ExecutableDirectory,
    #[error("desktop cache root has no application-data parent")]
    ApplicationDataDirectory,
    #[error("SHADOW_INFER_BASE_URL is not valid UTF-8")]
    InvalidInferBaseUrl,
}

#[cfg(test)]
mod tests;
