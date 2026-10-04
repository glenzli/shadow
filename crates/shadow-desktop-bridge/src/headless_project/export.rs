//! Headless PNG file adapter around the shared full-quality export service.
//! Native pixels retain display-sRGB precision; no resize, profile conversion,
//! metadata copying or alternate Recipe renderer is hidden in this boundary.

use std::{fs, io::Write, path::Path};

use anyhow::{Context, Result};
use serde_json::{Value, json};

use super::{fault, protocol::SnapshotGuard, verify_source};
use crate::{DesktopSession, ffi};

#[allow(clippy::too_many_arguments)]
pub(super) fn run(
    session: &DesktopSession,
    root: &Path,
    guard: &SnapshotGuard,
    source: &str,
    settings: ffi::FfiEditSettings,
    destination: &Path,
    bit_depth: u8,
) -> Result<Value> {
    if bit_depth != 8 && bit_depth != 16 {
        return Err(fault("unsupported", "PNG export bit_depth must be 8 or 16"));
    }
    if guard.base_commit_id.is_empty() {
        return Err(fault(
            "commit_required",
            "commit the captured draft before exporting; empty edits preserve a neutral photo",
        ));
    }
    if destination
        .extension()
        .and_then(|value| value.to_str())
        .is_none_or(|value| !value.eq_ignore_ascii_case("png"))
    {
        return Err(fault(
            "unsupported",
            "v1 exports PNG only; destination must have a .png extension",
        ));
    }
    let parent = destination
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."))
        .canonicalize()
        .context("resolve existing export directory")?;
    if parent.starts_with(root.canonicalize()?) {
        return Err(fault(
            "invalid_destination",
            "export outside the project directory to protect Catalog, originals and cache",
        ));
    }
    let name = destination
        .file_name()
        .ok_or_else(|| fault("invalid_destination", "destination needs a filename"))?;
    let destination = parent.join(name);
    match fs::symlink_metadata(&destination) {
        Ok(_) => {
            return Err(fault(
                "already_exists",
                "export never overwrites an existing destination",
            ));
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => (),
        Err(error) => return Err(error.into()),
    }
    let record = session
        .catalog
        .recipe_commit(guard.photo_id.parse()?, guard.base_commit_id.parse()?)?
        .ok_or_else(|| fault("commit_unavailable", "captured commit is unavailable"))?;
    let raster = session.render_basic_edit_export(
        &guard.photo_id,
        source,
        &ffi::FfiEditExportRequest {
            base_commit_id: guard.base_commit_id.clone(),
            settings,
            use_working_recipe: true,
            bit_depth,
        },
    )?;
    let mut staging = tempfile::NamedTempFile::new_in(&parent)?;
    encode_png(&mut staging, &raster)?;
    staging.flush()?;
    staging.as_file().sync_all()?;
    let output_sha256 = shadow_cache::sha256_file(staging.path())?;
    let output_bytes = staging.as_file().metadata()?.len();
    let (_, representation, digest) = verify_source(session, root, &guard.photo_id)?;
    if representation != guard.representation_id || digest != guard.source_sha256 {
        return Err(fault(
            "source_changed",
            "source changed during export; staged output discarded",
        ));
    }
    // Construct the complete receipt before publication. AssetLocation carries
    // lossless native bytes, including Unix paths that are not UTF-8; serializing
    // a PathBuf here could otherwise panic after the output had been published.
    let result = json!({
        "destination": shadow_core::native_location(&destination), "photo_id": guard.photo_id,
        "representation_id": guard.representation_id, "source_sha256": guard.source_sha256,
        "commit_id": guard.base_commit_id,
        "snapshot_digest": crate::digest_hex::encode_hex(&record.snapshot_digest),
        "width": raster.width, "height": raster.height, "bit_depth": bit_depth,
        "format": "png", "color_space": "display_srgb", "sampling": "native_full_resolution",
        "output_sha256": output_sha256, "output_bytes": output_bytes,
        "cancel": "unsupported"
    });
    // Publication is the final fallible step. Even a competing process or a
    // symlink created since preflight cannot make us replace existing bytes.
    staging.persist_noclobber(&destination).map_err(|error| {
        if error.error.kind() == std::io::ErrorKind::AlreadyExists {
            fault(
                "already_exists",
                "destination appeared during export; staged output discarded",
            )
        } else {
            error.error.into()
        }
    })?;
    Ok(result)
}

fn encode_png(output: impl Write, raster: &ffi::FfiEditedExportRaster) -> Result<()> {
    let mut encoder = png::Encoder::new(output, raster.width, raster.height);
    encoder.set_color(png::ColorType::Rgb);
    encoder.set_depth(match raster.bit_depth {
        8 => png::BitDepth::Eight,
        16 => png::BitDepth::Sixteen,
        _ => {
            return Err(fault(
                "operation_failed",
                "renderer returned an unsupported bit depth",
            ));
        }
    });
    encoder.set_source_srgb(png::SrgbRenderingIntent::RelativeColorimetric);
    let mut writer = encoder.write_header()?;
    if raster.bit_depth == 8 {
        writer.write_image_data(&raster.bytes)?;
    } else {
        // PNG stores 16-bit samples in network byte order. Preserve the exact
        // native high-bit export values; never quantize through an RGB8 image.
        let mut stream = writer.stream_writer()?;
        for row in raster.samples16.chunks(raster.width as usize * 3) {
            let bytes = row
                .iter()
                .flat_map(|sample| sample.to_be_bytes())
                .collect::<Vec<_>>();
            stream.write_all(&bytes)?;
        }
        stream.finish()?;
    }
    writer.finish()?;
    Ok(())
}
