//! Desktop filesystem layout for the side-loaded SAM provider.

use std::{
    ffi::OsString,
    path::{Path, PathBuf},
};

use thiserror::Error;

const PROVIDER_OVERRIDE: &str = "SHADOW_SAM2_COREML_PROVIDER_PATH";
const MODEL_OVERRIDE: &str = "SHADOW_SAM2_COREML_MODEL_DIR";
const MANIFEST_OVERRIDE: &str = "SHADOW_SAM2_COREML_MANIFEST_PATH";

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct SubjectMaskRuntimePaths {
    pub(crate) provider_executable: PathBuf,
    pub(crate) model_directory: PathBuf,
    pub(crate) manifest_path: PathBuf,
    pub(crate) scratch_root: PathBuf,
    /// Durable generated objects are deliberately a sibling of `cache/`.
    /// Ordinary preview-cache removal must never invalidate a persisted Recipe.
    pub(crate) derived_raster_store_root: PathBuf,
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
        let executable_directory = desktop_executable
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .ok_or(SubjectMaskRuntimePathError::ExecutableDirectory)?;
        let application_data_root = cache_root
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .ok_or(SubjectMaskRuntimePathError::ApplicationDataDirectory)?;

        let provider_executable = override_path(&environment, PROVIDER_OVERRIDE)
            .unwrap_or_else(|| executable_directory.join("shadow-sam2-coreml-provider"));
        let manifest_path = override_path(&environment, MANIFEST_OVERRIDE)
            .unwrap_or_else(|| executable_directory.join("shadow-sam2-coreml-model-manifest.json"));
        let model_directory = override_path(&environment, MODEL_OVERRIDE).unwrap_or_else(|| {
            application_data_root
                .join("models")
                .join("apple")
                .join("coreml-sam2.1-small")
        });
        Ok(Self {
            provider_executable,
            model_directory,
            manifest_path,
            scratch_root: cache_root.join("ai").join("sam2-coreml"),
            derived_raster_store_root: application_data_root.join("derived-rasters"),
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
}

#[cfg(test)]
mod tests;
