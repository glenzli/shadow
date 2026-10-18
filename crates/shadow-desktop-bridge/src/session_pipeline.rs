//! Exact single-file admission for the isolated desktop pipeline editor.
//!
//! A pipeline task receives one caller-selected source, not a folder.  It
//! registers that source only in the task-private Catalog so the established
//! Recipe, preview, detail, and export paths retain their identity checks
//! without creating Library state in the user's normal Shadow instance.

use std::{fs, path::Path, time::SystemTime};

use anyhow::{Context, Result as AnyResult, bail};
use shadow_catalog::{
    RecordDecodeSnapshot, RecordDecodeSnapshotStatus, RegisterAsset, RepresentationFingerprint,
};
use shadow_core::DecodeInspector;
use shadow_domain::RepresentationKind;

use crate::{native_path_ffi, photo_provider::PhotoInspector, wall_clock::current_time_ms};

use super::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn admit_pipeline_input(
        &self,
        source_path: &ffi::FfiNativePath,
    ) -> AnyResult<ffi::FfiPipelineInput> {
        let source_path = native_path_ffi::path_from_ffi(source_path)?;
        if representation_kind(&source_path).is_none() || !fs::metadata(&source_path)?.is_file() {
            bail!(
                "unsupported pipeline input format: {}",
                source_path.display()
            );
        }
        let source_path = if self
            .cache_root
            .parent()
            .is_some_and(|root| root.join(".shadow-document-snapshot-originals").is_file())
        {
            let root = self.cache_root.parent().expect("checked document root");
            let directory = root.join("sources").join(uuid::Uuid::now_v7().to_string());
            fs::create_dir_all(&directory)?;
            let target = directory.join(source_path.file_name().context("input filename missing")?);
            let metadata = fs::metadata(&source_path)?;
            anyhow::ensure!(
                metadata.len() <= 64 * 1024 * 1024 * 1024,
                "document source exceeds 64 GiB"
            );
            let mut copied = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&target)?;
            std::io::copy(&mut fs::File::open(&source_path)?, &mut copied)?;
            copied.set_times(std::fs::FileTimes::new().set_modified(metadata.modified()?))?;
            copied.sync_all()?;
            drop(copied);
            anyhow::ensure!(
                shadow_cache::sha256_file(&source_path)? == shadow_cache::sha256_file(&target)?,
                "original changed while creating its private snapshot"
            );
            let mut permissions = fs::metadata(&target)?.permissions();
            permissions.set_readonly(true);
            fs::set_permissions(&target, permissions)?;
            target
        } else {
            source_path
        };
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
        // Admission runs on the isolated session's worker. Inspect exactly the
        // selected source before publishing it as editable; do not scan its
        // directory or create a Library proxy. The observation also supplies
        // camera/optics facts needed by the ordinary Precision pipeline.
        let mut inspector =
            PhotoInspector::new_with_isolated_proxy_cache(Some(self.cache_root.clone()))?;
        let snapshot = inspector
            .inspect(&source_path)
            .map_err(anyhow::Error::msg)?;
        let current =
            fs::metadata(&source_path).context("recheck isolated input after inspection")?;
        let modified_at_ms = metadata.modified().ok().and_then(system_time_ms);
        if current.len() != metadata.len()
            || current.modified().ok().and_then(system_time_ms) != modified_at_ms
        {
            bail!("pipeline input changed during inspection");
        }
        let registered = self.catalog.register_asset(&RegisterAsset {
            kind,
            location: shadow_native_path::native_location(&source_path),
            byte_len: metadata.len(),
            modified_at_ms,
            now_ms: current_time_ms()?,
        })?;
        let recorded = self.catalog.record_decode_snapshot(&RecordDecodeSnapshot {
            representation_id: registered.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: metadata.len(),
                modified_at_ms,
            },
            snapshot,
            inspected_at_ms: current_time_ms()?,
        })?;
        if recorded != RecordDecodeSnapshotStatus::Recorded {
            bail!("pipeline input changed before metadata was recorded");
        }
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

pub(crate) fn representation_kind(path: &Path) -> Option<RepresentationKind> {
    let extension = path.extension()?.to_str()?.to_ascii_lowercase();
    match extension.as_str() {
        "nef" | "nrw" | "cr2" | "cr3" | "arw" | "raf" | "orf" | "rw2" | "pef" | "srw" | "dng" => {
            Some(RepresentationKind::OriginalRaw)
        }
        _ if shadow_bridge::photo_supported_raster_extensions().contains(&extension) => {
            Some(RepresentationKind::OriginalRaster)
        }
        _ => None,
    }
}

fn system_time_ms(time: SystemTime) -> Option<i64> {
    let duration = time.duration_since(SystemTime::UNIX_EPOCH).ok()?;
    i64::try_from(duration.as_millis()).ok()
}
