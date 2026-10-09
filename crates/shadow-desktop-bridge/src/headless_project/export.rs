//! Headless draft-preview and committed-export PNG publication around the
//! shared full-quality renderer. Pixels and their complete Recipe digest are
//! resolved together; draft artifacts never imply a history publication.

use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result};
use serde_json::{Value, json};

use super::{
    edit, fault, load_guard,
    protocol::{Edit, SnapshotGuard},
    verify_source,
};
use crate::{DesktopSession, ffi};

#[derive(Debug, Clone, Copy)]
pub(super) enum RecipeInput<'a> {
    Committed,
    Draft(&'a [Edit]),
}

pub(super) fn run(
    session: &DesktopSession,
    root: &Path,
    guard: &SnapshotGuard,
    input: RecipeInput<'_>,
    destination: &Path,
    bit_depth: u8,
) -> Result<Value> {
    let (source, mut settings) = load_guard(session, root, guard)?;
    let draft = matches!(input, RecipeInput::Draft(_));
    match input {
        RecipeInput::Committed if guard.base_commit_id.is_empty() => {
            return Err(fault(
                "commit_required",
                "commit the captured draft before exporting; use preview to inspect without committing",
            ));
        }
        RecipeInput::Draft(edits) => edit::apply(&mut settings, edits)?,
        RecipeInput::Committed => (),
    }
    let destination = checked_destination(root, destination, bit_depth)?;
    let immutable_digest = if draft {
        None
    } else {
        Some(committed_snapshot_digest(session, guard)?)
    };
    let rendered = session.render_basic_edit_export_receipt(
        &guard.photo_id,
        &source,
        &ffi::FfiEditExportRequest {
            base_commit_id: guard.base_commit_id.clone(),
            settings,
            use_working_recipe: true,
            bit_depth,
        },
    )?;
    let mut staging = tempfile::NamedTempFile::new_in(destination.parent().unwrap())?;
    encode_png(&mut staging, &rendered.raster)?;
    staging.flush()?;
    staging.as_file().sync_all()?;
    let output_sha256 = shadow_cache::sha256_file(staging.path())?;
    let output_bytes = staging.as_file().metadata()?.len();
    let (_, representation, digest) = verify_source(session, root, &guard.photo_id)?;
    if representation != guard.representation_id || digest != guard.source_sha256 {
        return Err(fault(
            "source_changed",
            "managed original changed during rendering; staged output discarded",
        ));
    }
    // Serialize before publication, using lossless native locations even when
    // the canonical parent of a UTF-8 request is not representable as UTF-8.
    let raster = rendered.raster;
    let mut result = json!({
        "destination": shadow_core::native_location(&destination), "photo_id": guard.photo_id,
        "representation_id": guard.representation_id, "source_sha256": guard.source_sha256,
        "snapshot_digest": crate::digest_hex::encode_hex(&immutable_digest.unwrap_or(rendered.snapshot_digest)),
        "snapshot_digest_role": if draft { "rendered_draft" } else { "immutable_commit" },
        "rendered_snapshot_digest": crate::digest_hex::encode_hex(&rendered.snapshot_digest),
        "width": raster.width, "height": raster.height, "bit_depth": bit_depth,
        "format": "png", "color_space": "display_srgb", "sampling": "native_full_resolution",
        "output_sha256": output_sha256, "output_bytes": output_bytes,
        "cancel": "unsupported", "committed": !draft
    });
    if draft {
        result["artifact_kind"] = json!("draft_preview");
        result["base_commit_id"] = json!(guard.base_commit_id);
        result["guard"] = json!(guard);
    } else {
        result["artifact_kind"] = json!("committed_export");
        result["commit_id"] = json!(guard.base_commit_id);
    }
    // The final fallible step cannot overwrite files or symlinks created after
    // preflight. A preview deliberately reserves no authored working-head state.
    staging.persist_noclobber(&destination).map_err(|error| {
        if error.error.kind() == std::io::ErrorKind::AlreadyExists {
            fault(
                "already_exists",
                "destination appeared during rendering; staged output discarded",
            )
        } else {
            error.error.into()
        }
    })?;
    Ok(result)
}

fn committed_snapshot_digest(session: &DesktopSession, guard: &SnapshotGuard) -> Result<[u8; 32]> {
    // Supported legacy Recipes can normalize during editable projection. Keep
    // the established export receipt's immutable commit identity independent
    // of the canonical snapshot used by the actual renderer.
    Ok(session
        .catalog
        .recipe_commit(guard.photo_id.parse()?, guard.base_commit_id.parse()?)?
        .ok_or_else(|| fault("commit_unavailable", "captured commit is unavailable"))?
        .snapshot_digest)
}

fn checked_destination(root: &Path, destination: &Path, bit_depth: u8) -> Result<PathBuf> {
    if bit_depth != 8 && bit_depth != 16 {
        return Err(fault("unsupported", "PNG bit_depth must be 8 or 16"));
    }
    if destination
        .extension()
        .and_then(|value| value.to_str())
        .is_none_or(|value| !value.eq_ignore_ascii_case("png"))
    {
        return Err(fault(
            "unsupported",
            "v1 writes PNG only; destination must have a .png extension",
        ));
    }
    let parent = destination
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."))
        .canonicalize()
        .context("resolve existing output directory")?;
    if parent.starts_with(root.canonicalize()?) {
        return Err(fault(
            "invalid_destination",
            "write outside the project directory to protect Catalog, originals and cache",
        ));
    }
    let name = destination
        .file_name()
        .ok_or_else(|| fault("invalid_destination", "destination needs a filename"))?;
    let destination = parent.join(name);
    match fs::symlink_metadata(&destination) {
        Ok(_) => Err(fault(
            "already_exists",
            "output never overwrites an existing destination",
        )),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(destination),
        Err(error) => Err(error.into()),
    }
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
