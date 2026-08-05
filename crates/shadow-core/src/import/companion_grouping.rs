//! Conservative source-local grouping for camera RAW and companion JPEG files.

use std::path::Path;

use shadow_catalog::{CatalogError, ImportPhotoGrouping};
use shadow_domain::RepresentationKind;

/// Returns a stable source-relative key for conventional camera companions.
///
/// RAW files participate so a JPEG discovered first can remain the canonical
/// logical photo when its RAW arrives later. Raster participation is limited
/// to JPEG; TIFF, PNG, HEIF, and other likely derived files must not be merged
/// merely because an export reused the source stem.
pub(super) fn grouping_for_path(
    root: &Path,
    path: &Path,
    kind: RepresentationKind,
) -> Result<Option<ImportPhotoGrouping>, CatalogError> {
    if !participates(path, kind) {
        return Ok(None);
    }
    let Some(relative) = path.strip_prefix(root).ok() else {
        return Ok(None);
    };
    let Some(stem) = relative.file_stem().and_then(|value| value.to_str()) else {
        return Ok(None);
    };
    if stem.is_empty() {
        return Ok(None);
    }
    let Some(parent) = relative.parent().and_then(Path::to_str) else {
        return Ok(None);
    };
    let normalized_stem = stem.to_lowercase();
    let normalized_parent = parent.replace('\\', "/").to_lowercase();
    let key = if normalized_parent.is_empty() {
        normalized_stem
    } else {
        format!("{normalized_parent}/{normalized_stem}")
    };
    ImportPhotoGrouping::same_directory_stem(key).map(Some)
}

fn participates(path: &Path, kind: RepresentationKind) -> bool {
    match kind {
        RepresentationKind::OriginalRaw => true,
        RepresentationKind::OriginalRaster => path
            .extension()
            .and_then(|value| value.to_str())
            .is_some_and(|extension| {
                extension.eq_ignore_ascii_case("jpg") || extension.eq_ignore_ascii_case("jpeg")
            }),
        _ => false,
    }
}

#[cfg(test)]
mod tests;
