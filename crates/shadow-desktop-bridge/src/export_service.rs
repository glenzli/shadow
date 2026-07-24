//! Full-resolution, Recipe-exact export raster preparation.
//!
//! File formats, destination naming, presets, and watermark composition are
//! desktop-shell concerns. This service owns the expensive invariant: source
//! development uses `ExportImage` intent and the exact current Recipe, then
//! returns one tightly packed display-sRGB RGB8 raster.

use super::*;

impl DesktopSession {
    pub(crate) fn render_basic_edit_export(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditExportRequest,
    ) -> AnyResult<ffi::FfiEditedExportRaster> {
        const EXPORT_TILE_SIDE: u32 = 1_024;
        const SOURCE_CHANGED: &str = "export source changed since Catalog registration";

        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let native_path = catalog_native_path(&source)?;
        if fingerprint_source(&native_path).context("read export source metadata")? != source.source
        {
            bail!(SOURCE_CHANGED);
        }
        let (plan, _) = self.basic_edit_render_plan_with_identity(
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        let raw_plan = RawDevelopmentPlan::export_image();
        let optics = bridge_optics_settings(&request.settings.optics);
        let session = match PhotoEditDetailSession::open_with_raw_development_plan_and_optics(
            &native_path,
            raw_plan,
            &optics,
        ) {
            Ok(session) => session,
            Err(public_decoder_error) => {
                let temporary_raster =
                    isolated_edit_raster(&self.cache_root, &native_path, 16_384).with_context(
                        || {
                            format!(
                                "public decoder could not prepare export for {}; isolated decoder fallback could not start: {public_decoder_error}",
                                native_path.display()
                            )
                        },
                    )?;
                let isolated_result =
                    PhotoEditDetailSession::open_with_raw_development_plan_and_optics(
                        &temporary_raster,
                        raw_plan,
                        &optics,
                    );
                let _ = std::fs::remove_file(&temporary_raster);
                isolated_result.with_context(|| {
                    format!(
                        "public decoder could not prepare export for {}; isolated decoder fallback also failed: {public_decoder_error}",
                        native_path.display()
                    )
                })?
            }
        };
        let dimensions = session.dimensions();
        let row_stride_bytes = dimensions
            .width
            .checked_mul(3)
            .ok_or_else(|| anyhow!("export row stride overflowed"))?;
        let byte_len = u64::from(row_stride_bytes)
            .checked_mul(u64::from(dimensions.height))
            .and_then(|value| usize::try_from(value).ok())
            .ok_or_else(|| anyhow!("export raster allocation overflowed"))?;
        let mut bytes = vec![0_u8; byte_len];
        let mut y = 0;
        while y < dimensions.height {
            let mut x = 0;
            while x < dimensions.width {
                let rect = DetailTileRect {
                    x,
                    y,
                    width: EXPORT_TILE_SIDE.min(dimensions.width - x),
                    height: EXPORT_TILE_SIDE.min(dimensions.height - y),
                };
                let tile = session.render_plan_tile(&plan, DetailTileRequest { rect })?;
                let tile_stride =
                    usize::try_from(tile.row_stride_bytes).context("convert export tile stride")?;
                let destination_stride =
                    usize::try_from(row_stride_bytes).context("convert export row stride")?;
                let destination_x = usize::try_from(x)
                    .context("convert export tile x")?
                    .checked_mul(3)
                    .ok_or_else(|| anyhow!("export tile x overflowed"))?;
                for row in 0..usize::try_from(rect.height).context("convert export tile height")? {
                    let source_start = row
                        .checked_mul(tile_stride)
                        .ok_or_else(|| anyhow!("export tile row overflowed"))?;
                    let source_end = source_start
                        .checked_add(tile_stride)
                        .ok_or_else(|| anyhow!("export tile row end overflowed"))?;
                    let destination_row = usize::try_from(y)
                        .context("convert export tile y")?
                        .checked_add(row)
                        .and_then(|value| value.checked_mul(destination_stride))
                        .and_then(|value| value.checked_add(destination_x))
                        .ok_or_else(|| anyhow!("export destination row overflowed"))?;
                    let destination_end = destination_row
                        .checked_add(tile_stride)
                        .ok_or_else(|| anyhow!("export destination row end overflowed"))?;
                    bytes[destination_row..destination_end]
                        .copy_from_slice(&tile.bytes[source_start..source_end]);
                }
                x = x
                    .checked_add(rect.width)
                    .ok_or_else(|| anyhow!("export tile x advance overflowed"))?;
            }
            y = y
                .checked_add(EXPORT_TILE_SIDE.min(dimensions.height - y))
                .ok_or_else(|| anyhow!("export tile y advance overflowed"))?;
        }
        if fingerprint_source(&native_path).context("re-read export source metadata")?
            != source.source
        {
            bail!(SOURCE_CHANGED);
        }
        Ok(ffi::FfiEditedExportRaster {
            width: dimensions.width,
            height: dimensions.height,
            row_stride_bytes,
            bytes,
        })
    }
}

#[cfg(test)]
mod tests {
    use std::path::PathBuf;

    use shadow_catalog::RegisterAsset;
    use shadow_core::{fingerprint_source, native_location};
    use shadow_domain::{RepresentationId, RepresentationKind};

    use super::*;

    #[test]
    #[ignore = "requires SHADOW_TEST_EXPORT_RAW to name a local RAW fixture"]
    fn real_raw_export_renders_a_tightly_packed_full_resolution_raster() {
        let source_path =
            PathBuf::from(std::env::var_os("SHADOW_TEST_EXPORT_RAW").expect("fixture path"));
        let source = fingerprint_source(&source_path).expect("fingerprint export fixture");
        let root = std::env::temp_dir().join(format!(
            "shadow-export-smoke-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create export smoke root");
        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("open export smoke session");
        let registered = session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: native_location(&source_path),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 1,
            })
            .expect("register export fixture");
        let photo_id = registered.photo_id.to_string();
        let source_path = source_path.to_str().expect("UTF-8 fixture path");
        let state = session
            .photo_edit_state(&photo_id, source_path)
            .expect("prepare neutral working Recipe");
        let raster = session
            .render_basic_edit_export(
                &photo_id,
                source_path,
                &ffi::FfiEditExportRequest {
                    base_commit_id: state.working_commit_id,
                    settings: state.settings,
                    use_working_recipe: true,
                },
            )
            .expect("render full-resolution export");

        assert!(raster.width > 0);
        assert!(raster.height > 0);
        assert_eq!(raster.row_stride_bytes, raster.width * 3);
        assert_eq!(
            raster.bytes.len(),
            usize::try_from(u64::from(raster.row_stride_bytes) * u64::from(raster.height))
                .expect("export byte length")
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove export smoke root");
    }
}
