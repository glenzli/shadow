//! Exact single-file admission for the isolated desktop pipeline editor.
//!
//! A pipeline task receives one caller-selected source, not a folder.  It
//! registers that source only in the task-private Catalog so the established
//! Recipe, preview, detail, and export paths retain their identity checks
//! without creating Library state in the user's normal Shadow instance.

use std::{fs, path::Path, time::SystemTime};

use anyhow::{Context, Result as AnyResult, bail};
use shadow_catalog::RegisterAsset;
use shadow_domain::RepresentationKind;

use crate::{native_path_ffi, wall_clock::current_time_ms};

use super::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn admit_pipeline_input(
        &self,
        source_path: &ffi::FfiNativePath,
    ) -> AnyResult<ffi::FfiPipelineInput> {
        let source_path = native_path_ffi::path_from_ffi(source_path)?;
        let metadata = fs::metadata(&source_path)
            .with_context(|| format!("read pipeline input {}", source_path.display()))?;
        if !metadata.is_file() {
            bail!(
                "pipeline input is not a regular file: {}",
                source_path.display()
            );
        }
        let kind = representation_kind(&source_path).ok_or_else(|| {
            anyhow::anyhow!(
                "unsupported pipeline input format: {}",
                source_path.display()
            )
        })?;
        let modified_at_ms = metadata.modified().ok().and_then(system_time_ms);
        let registered = self.catalog.register_asset(&RegisterAsset {
            kind,
            location: shadow_native_path::native_location(&source_path),
            byte_len: metadata.len(),
            modified_at_ms,
            now_ms: current_time_ms()?,
        })?;
        let title = source_path
            .file_stem()
            .and_then(|name| name.to_str())
            .unwrap_or("Shadow")
            .to_owned();
        Ok(ffi::FfiPipelineInput {
            photo_id: registered.photo_id.to_string(),
            representation_id: registered.representation_id.to_string(),
            source_path: source_path.display().to_string(),
            title,
        })
    }
}

fn representation_kind(path: &Path) -> Option<RepresentationKind> {
    let extension = path.extension()?.to_str()?.to_ascii_lowercase();
    match extension.as_str() {
        "nef" | "nrw" | "cr2" | "cr3" | "arw" | "raf" | "orf" | "rw2" | "pef" | "srw" | "dng" => {
            Some(RepresentationKind::OriginalRaw)
        }
        "jpg" | "jpeg" | "tif" | "tiff" | "png" | "heic" | "heif" => {
            Some(RepresentationKind::OriginalRaster)
        }
        _ => None,
    }
}

fn system_time_ms(time: SystemTime) -> Option<i64> {
    let duration = time.duration_since(SystemTime::UNIX_EPOCH).ok()?;
    i64::try_from(duration.as_millis()).ok()
}
