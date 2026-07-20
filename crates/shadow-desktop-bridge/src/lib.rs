//! Coarse-grained Rust snapshot boundary consumed by the Qt desktop shell.

use std::path::{Path, PathBuf};

use anyhow::{Context, Result as AnyResult};
use shadow_bridge::{
    extract_best_libraw_preview, inspect_libraw, libraw_provider_version,
    render_libraw_reference_proxy,
};
use shadow_catalog::{CachedArtifactRole, CatalogActor};
use shadow_core::{
    CachedArtifactLoader, DecodeInspectionActor, DecodeInspector, scan_folder_with_inspection,
};
use shadow_domain::{DecoderSnapshot, PreviewPayload, ProxyPayload};

#[cxx::bridge(namespace = "shadow::desktop")]
mod ffi {
    #[derive(Debug)]
    struct FfiReviewItem {
        photo_id: String,
        representation_id: String,
        title: String,
        source_path: String,
        visual_role: String,
        visual_width: u32,
        visual_height: u32,
        visual_bytes: Vec<u8>,
        visual_error: String,
    }

    #[derive(Debug)]
    struct FfiReviewSnapshot {
        folder_path: String,
        files_seen: u64,
        supported_files: u64,
        decode_inspections_queued: u64,
        issue_count: u64,
        items: Vec<FfiReviewItem>,
    }

    extern "Rust" {
        fn scan_review(
            catalog_path: &str,
            cache_root: &str,
            folder_path: &str,
        ) -> Result<FfiReviewSnapshot>;
    }
}

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
        "libraw:grid-jpeg-2048-q88-v1"
    }
}

fn scan_review(
    catalog_path: &str,
    cache_root: &str,
    folder_path: &str,
) -> AnyResult<ffi::FfiReviewSnapshot> {
    scan_review_inner(
        Path::new(catalog_path),
        Path::new(cache_root),
        Path::new(folder_path),
    )
}

fn scan_review_inner(
    catalog_path: &Path,
    cache_root: &Path,
    folder_path: &Path,
) -> AnyResult<ffi::FfiReviewSnapshot> {
    ensure_parent(catalog_path)?;
    let actor = CatalogActor::spawn(catalog_path)
        .with_context(|| format!("open catalog {}", catalog_path.display()))?;
    let mut catalog = actor.handle();
    let inspector = DecodeInspectionActor::spawn_with_cache(
        catalog.clone(),
        LibRawInspector::new(),
        cache_root,
    )?;
    let report = scan_folder_with_inspection(&mut catalog, &inspector.handle(), folder_path)
        .with_context(|| format!("scan {}", folder_path.display()))?;
    inspector.shutdown()?;

    let loader = CachedArtifactLoader::open(catalog.clone(), cache_root)?;
    let mut items = Vec::new();
    for record in catalog.review_items()? {
        let (visual_role, visual_width, visual_height, visual_bytes, visual_error) =
            if let Some(visual) = record.visual {
                let role = match visual.artifact.role {
                    CachedArtifactRole::EmbeddedPreview => "embedded",
                    CachedArtifactRole::GeneratedProxy => "proxy",
                };
                match loader.load_bytes(&visual) {
                    Ok(bytes) => (
                        role.to_owned(),
                        visual.artifact.dimensions.width,
                        visual.artifact.dimensions.height,
                        bytes,
                        String::new(),
                    ),
                    Err(error) => (
                        role.to_owned(),
                        visual.artifact.dimensions.width,
                        visual.artifact.dimensions.height,
                        Vec::new(),
                        error.to_string(),
                    ),
                }
            } else {
                (String::new(), 0, 0, Vec::new(), "visual pending".into())
            };
        items.push(ffi::FfiReviewItem {
            photo_id: record.photo_id.to_string(),
            representation_id: record.representation_id.to_string(),
            title: file_name(&record.location.display_path),
            source_path: record.location.display_path,
            visual_role,
            visual_width,
            visual_height,
            visual_bytes,
            visual_error,
        });
    }
    let snapshot = ffi::FfiReviewSnapshot {
        folder_path: folder_path.display().to_string(),
        files_seen: report.files_seen,
        supported_files: report.supported_files,
        decode_inspections_queued: report.decode_inspections_queued,
        issue_count: u64::try_from(report.issues.len()).unwrap_or(u64::MAX),
        items,
    };
    actor.shutdown()?;
    Ok(snapshot)
}

fn ensure_parent(path: &Path) -> AnyResult<()> {
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create catalog directory {}", parent.display()))?;
    }
    Ok(())
}

fn file_name(display_path: &str) -> String {
    PathBuf::from(display_path)
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or(display_path)
        .to_owned()
}

#[cfg(test)]
mod tests {
    use shadow_domain::{EntityId, RepresentationId};

    use super::*;

    #[test]
    fn display_title_uses_the_final_path_component() {
        assert_eq!(file_name("/photos/trip/input.dng"), "input.dng");
        assert_eq!(file_name("input.dng"), "input.dng");
    }

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG_FOLDER to contain local RAW fixtures"]
    fn real_dng_folder_builds_a_review_snapshot_for_qt() {
        let folder = std::env::var_os("SHADOW_TEST_DNG_FOLDER").expect("SHADOW_TEST_DNG_FOLDER");
        let root = std::env::temp_dir().join(format!(
            "shadow-desktop-bridge-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create desktop bridge fixture");
        let snapshot = scan_review_inner(
            &root.join("catalog.sqlite"),
            &root.join("cache"),
            Path::new(&folder),
        )
        .expect("build real Review snapshot");

        assert!(snapshot.supported_files >= 2);
        assert!(snapshot.items.len() >= 2);
        assert!(snapshot.items.iter().all(|item| {
            item.visual_bytes.starts_with(&[0xff, 0xd8])
                && item.visual_bytes.ends_with(&[0xff, 0xd9])
        }));
        std::fs::remove_dir_all(root).expect("remove desktop bridge fixture");
    }
}
