//! Shared credential location for desktop Infer Runtime consumers.

use std::path::{Path, PathBuf};

/// Qt may repeat the organization and application name in its data root.
/// Credentials belong beside that application directory, as in the native shell.
pub(crate) fn default_credential_file(application_data_root: &Path) -> PathBuf {
    let root = application_data_root.parent().filter(|parent| {
        application_data_root
            .file_name()
            .zip(parent.file_name())
            .is_some_and(|(application, organization)| application == organization)
    });
    root.unwrap_or(application_data_root)
        .join("credentials")
        .join("infer-runtime-shadow.token")
}

#[cfg(test)]
mod tests;
