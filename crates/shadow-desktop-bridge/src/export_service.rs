//! Full-resolution, Recipe-exact export raster preparation.
//!
//! File formats, destination naming, presets, and watermark composition are
//! desktop-shell concerns. This service owns the expensive invariant: source
//! development uses `ExportImage` intent and the exact current Recipe, then
//! returns one tightly packed display-sRGB RGB8 or RGB16 raster.

use std::path::Path;

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_ai::CancellationToken;
use shadow_bridge::{
    AdjustmentRenderPlan, DetailSessionRequirements, DetailTileRect, DetailTileRequest,
    OpticsSettings, PhotoEditDetailSession, RawDevelopmentPlan,
};
use shadow_core::fingerprint_source;

use crate::isolated_proxy::{
    NativeDecodeAdmission, configured_helper_path, native_decode_admission_after_isolated_stages,
    stage_isolated_raw_frame,
};
use crate::{
    DesktopSession, ffi,
    raw_foundation_render_source::{
        RawFoundationRenderSelection, load_raw_foundation_for_render,
        raw_foundation_ready_for_render,
    },
    recipe_v1::{ensure_foundation_development_receipt, resolve_recipe_render},
    session_photo_source::catalog_native_path,
};

const EXPORT_TILE_SIDE: u32 = 1_024;
const SOURCE_CHANGED: &str = "export source changed since Catalog registration";

impl DesktopSession {
    pub(crate) fn render_basic_edit_export(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditExportRequest,
    ) -> AnyResult<ffi::FfiEditedExportRaster> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let native_path = catalog_native_path(&source)?;
        if fingerprint_source(&native_path).context("read export source metadata")? != source.source
        {
            bail!(SOURCE_CHANGED);
        }
        let recipe = resolve_recipe_render(
            &self.catalog,
            &self.cache_root,
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        let raw_development_plan = recipe.foundation.export_plan();
        if request.bit_depth != 8 && request.bit_depth != 16 {
            bail!("export bit depth must be 8 or 16");
        }
        let requirements = if request.bit_depth == 16 {
            DetailSessionRequirements::for_high_bit_export(&recipe.plan)
        } else {
            DetailSessionRequirements::for_render_plan(&recipe.plan)
        };
        // Export has no cancellation surface today. Supplying an explicit
        // token keeps source resolution on the same contract as preview and
        // detail without pretending that the queue can currently signal it.
        let foundation_cancellation = CancellationToken::default();
        let raw_foundation = raw_foundation_ready_for_render(
            &self.raw_foundations,
            &self.raw_foundation_runtime,
            &native_path,
            source.source,
            recipe.foundation.raw_ai_denoise(),
            &foundation_cancellation,
        )?;
        ensure_known_quarantined_raw_does_not_open_for_export(&self.cache_root, &native_path)?;
        let session = open_export_session(
            &self.cache_root,
            &native_path,
            raw_development_plan,
            recipe.foundation.optics(),
            raw_foundation.as_ref(),
            source.source,
            requirements,
        )?;
        let raster = render_export_raster(&session, &recipe.plan, request.bit_depth)?;
        if fingerprint_source(&native_path).context("re-read export source metadata")?
            != source.source
        {
            bail!(SOURCE_CHANGED);
        }
        Ok(raster)
    }
}

fn open_export_session(
    cache_root: &Path,
    native_path: &Path,
    raw_plan: RawDevelopmentPlan,
    optics: &OpticsSettings,
    raw_foundation: Option<&RawFoundationRenderSelection>,
    source: shadow_catalog::RepresentationFingerprint,
    requirements: DetailSessionRequirements,
) -> AnyResult<PhotoEditDetailSession> {
    if let Some(selection) = raw_foundation {
        let loaded = load_raw_foundation_for_render(selection, native_path, source)?;
        let session = match loaded.staging_manifest_path.as_deref() {
            Some(staging_manifest) => PhotoEditDetailSession::open_with_staged_raw_foundation(
                native_path,
                staging_manifest,
                raw_plan,
                &loaded.foundation,
                optics,
                requirements,
            ),
            None => PhotoEditDetailSession::open_with_raw_foundation(
                native_path,
                raw_plan,
                &loaded.foundation,
                optics,
                requirements,
            ),
        }
        .context("prepare export from verified AI RAW foundation")?;
        ensure_foundation_development_receipt(raw_plan, session.raw_pipeline_receipt())?;
        return Ok(session);
    }
    match PhotoEditDetailSession::open_with_requirements(
        native_path,
        raw_plan,
        optics,
        requirements,
    ) {
        Ok(session) => {
            ensure_foundation_development_receipt(raw_plan, session.raw_pipeline_receipt())?;
            Ok(session)
        }
        Err(public_decoder_error) => {
            let helper_path = configured_helper_path().ok_or_else(|| {
                anyhow!(
                    "full-quality RAW export requires the isolated Provider Host RawFrame route; public decoder could not prepare export for {}: {public_decoder_error}",
                    native_path.display()
                )
            })?;
            let staging_root = cache_root.join("decode-helper").join("raw-frame-staging");
            let staging = stage_isolated_raw_frame(&helper_path, &staging_root, native_path)
                .with_context(|| {
                    format!(
                        "stage provider-neutral RawFrame for export after public decoder could not prepare {}: {public_decoder_error}",
                        native_path.display()
                    )
                })?;
            let session = PhotoEditDetailSession::open_with_staged_raw_development_plan(
                native_path,
                staging.manifest_path(),
                raw_plan,
                optics,
                requirements,
            )
            .with_context(|| {
                format!(
                    "prepare full-quality export from the isolated RawFrame for {}",
                    native_path.display()
                )
            })?;
            ensure_foundation_development_receipt(raw_plan, session.raw_pipeline_receipt())?;
            Ok(session)
        }
    }
}

fn render_export_raster(
    session: &PhotoEditDetailSession,
    plan: &AdjustmentRenderPlan,
    bit_depth: u8,
) -> AnyResult<ffi::FfiEditedExportRaster> {
    match bit_depth {
        8 => render_export_raster8(session, plan),
        16 => render_export_raster16(session, plan),
        _ => bail!("export bit depth must be 8 or 16"),
    }
}

fn render_export_raster8(
    session: &PhotoEditDetailSession,
    plan: &AdjustmentRenderPlan,
) -> AnyResult<ffi::FfiEditedExportRaster> {
    let dimensions = plan.geometry.output_dimensions(session.dimensions())?;
    let row_stride_bytes = dimensions
        .width
        .checked_mul(3)
        .ok_or_else(|| anyhow!("export row stride overflowed"))?;
    let byte_len = u64::from(row_stride_bytes)
        .checked_mul(u64::from(dimensions.height))
        .and_then(|value| usize::try_from(value).ok())
        .ok_or_else(|| anyhow!("export raster allocation overflowed"))?;
    let mut bytes = vec![0_u8; byte_len];
    let destination_stride =
        usize::try_from(row_stride_bytes).context("convert export row stride")?;
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
            let tile = session.render_plan_tile(plan, DetailTileRequest { rect })?;
            copy_export_tile(
                &mut bytes,
                destination_stride,
                &ExportTileCopy {
                    origin_x: x,
                    origin_y: y,
                    rect,
                    source: &tile.bytes,
                    source_row_stride_bytes: tile.row_stride_bytes,
                },
            )?;
            x = x
                .checked_add(rect.width)
                .ok_or_else(|| anyhow!("export tile x advance overflowed"))?;
        }
        y = y
            .checked_add(EXPORT_TILE_SIDE.min(dimensions.height - y))
            .ok_or_else(|| anyhow!("export tile y advance overflowed"))?;
    }
    Ok(ffi::FfiEditedExportRaster {
        width: dimensions.width,
        height: dimensions.height,
        row_stride_bytes,
        bit_depth: 8,
        bytes,
        samples16: Vec::new(),
    })
}

fn render_export_raster16(
    session: &PhotoEditDetailSession,
    plan: &AdjustmentRenderPlan,
) -> AnyResult<ffi::FfiEditedExportRaster> {
    let dimensions = plan.geometry.output_dimensions(session.dimensions())?;
    let row_stride_samples = dimensions
        .width
        .checked_mul(3)
        .ok_or_else(|| anyhow!("RGB16 export sample stride overflowed"))?;
    let row_stride_bytes = row_stride_samples
        .checked_mul(2)
        .ok_or_else(|| anyhow!("RGB16 export byte stride overflowed"))?;
    let sample_len = u64::from(row_stride_samples)
        .checked_mul(u64::from(dimensions.height))
        .and_then(|value| usize::try_from(value).ok())
        .ok_or_else(|| anyhow!("RGB16 export raster allocation overflowed"))?;
    let mut samples = vec![0_u16; sample_len];
    let destination_stride =
        usize::try_from(row_stride_samples).context("convert RGB16 export sample stride")?;
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
            let tile = session.render_plan_tile16(plan, DetailTileRequest { rect })?;
            copy_export_tile16(
                &mut samples,
                destination_stride,
                &ExportTileCopy16 {
                    origin_x: x,
                    origin_y: y,
                    rect,
                    source: &tile.samples,
                    source_row_stride_bytes: tile.row_stride_bytes,
                },
            )?;
            x = x
                .checked_add(rect.width)
                .ok_or_else(|| anyhow!("RGB16 export tile x advance overflowed"))?;
        }
        y = y
            .checked_add(EXPORT_TILE_SIDE.min(dimensions.height - y))
            .ok_or_else(|| anyhow!("RGB16 export tile y advance overflowed"))?;
    }
    Ok(ffi::FfiEditedExportRaster {
        width: dimensions.width,
        height: dimensions.height,
        row_stride_bytes,
        bit_depth: 16,
        bytes: Vec::new(),
        samples16: samples,
    })
}

struct ExportTileCopy<'a> {
    origin_x: u32,
    origin_y: u32,
    rect: DetailTileRect,
    source: &'a [u8],
    source_row_stride_bytes: u32,
}

fn copy_export_tile(
    destination: &mut [u8],
    destination_stride: usize,
    tile: &ExportTileCopy<'_>,
) -> AnyResult<()> {
    let source_stride =
        usize::try_from(tile.source_row_stride_bytes).context("convert export tile stride")?;
    let destination_x = usize::try_from(tile.origin_x)
        .context("convert export tile x")?
        .checked_mul(3)
        .ok_or_else(|| anyhow!("export tile x overflowed"))?;
    for row in 0..usize::try_from(tile.rect.height).context("convert export tile height")? {
        let source_start = row
            .checked_mul(source_stride)
            .ok_or_else(|| anyhow!("export tile row overflowed"))?;
        let source_end = source_start
            .checked_add(source_stride)
            .ok_or_else(|| anyhow!("export tile row end overflowed"))?;
        let destination_row = usize::try_from(tile.origin_y)
            .context("convert export tile y")?
            .checked_add(row)
            .and_then(|value| value.checked_mul(destination_stride))
            .and_then(|value| value.checked_add(destination_x))
            .ok_or_else(|| anyhow!("export destination row overflowed"))?;
        let destination_end = destination_row
            .checked_add(source_stride)
            .ok_or_else(|| anyhow!("export destination row end overflowed"))?;
        destination[destination_row..destination_end]
            .copy_from_slice(&tile.source[source_start..source_end]);
    }
    Ok(())
}

struct ExportTileCopy16<'a> {
    origin_x: u32,
    origin_y: u32,
    rect: DetailTileRect,
    source: &'a [u16],
    source_row_stride_bytes: u32,
}

fn copy_export_tile16(
    destination: &mut [u16],
    destination_stride: usize,
    tile: &ExportTileCopy16<'_>,
) -> AnyResult<()> {
    if tile.source_row_stride_bytes % 2 != 0 {
        bail!("RGB16 export tile row stride is not sample-aligned");
    }
    let source_stride = usize::try_from(tile.source_row_stride_bytes / 2)
        .context("convert RGB16 export tile stride")?;
    let expected_source_stride = usize::try_from(tile.rect.width)
        .context("convert RGB16 export tile width")?
        .checked_mul(3)
        .ok_or_else(|| anyhow!("RGB16 export tile width overflowed"))?;
    if source_stride != expected_source_stride {
        bail!("RGB16 export tile stride does not match its packed width");
    }
    let destination_x = usize::try_from(tile.origin_x)
        .context("convert RGB16 export tile x")?
        .checked_mul(3)
        .ok_or_else(|| anyhow!("RGB16 export tile x overflowed"))?;
    for row in 0..usize::try_from(tile.rect.height).context("convert RGB16 export tile height")? {
        let source_start = row
            .checked_mul(source_stride)
            .ok_or_else(|| anyhow!("RGB16 export tile row overflowed"))?;
        let source_end = source_start
            .checked_add(source_stride)
            .ok_or_else(|| anyhow!("RGB16 export tile row end overflowed"))?;
        let destination_row = usize::try_from(tile.origin_y)
            .context("convert RGB16 export tile y")?
            .checked_add(row)
            .and_then(|value| value.checked_mul(destination_stride))
            .and_then(|value| value.checked_add(destination_x))
            .ok_or_else(|| anyhow!("RGB16 export destination row overflowed"))?;
        let destination_end = destination_row
            .checked_add(source_stride)
            .ok_or_else(|| anyhow!("RGB16 export destination row end overflowed"))?;
        destination[destination_row..destination_end]
            .copy_from_slice(&tile.source[source_start..source_end]);
    }
    Ok(())
}

/// A known child-process crash or timeout is evidence that this exact source
/// revision must not be opened by the desktop's native full-detail exporter.
///
/// This is deliberately a circuit breaker, not a positive safety proof:
/// `NotQuarantined` preserves the existing public-decoder attempt for sources
/// with no negative observation. Full first-open isolation requires a later
/// high-bit-depth worker protocol, rather than using an 8-bit proxy as a false
/// substitute for a RAW export.
fn ensure_known_quarantined_raw_does_not_open_for_export(
    cache_root: &std::path::Path,
    source_path: &std::path::Path,
) -> AnyResult<()> {
    let Some(helper_path) = configured_helper_path() else {
        // Non-desktop tests and public-only deployments retain the existing
        // public decoder route. The safety cache is meaningful only alongside
        // the configured helper that produced its observations.
        return Ok(());
    };
    reject_known_quarantined_raw_export(native_decode_admission_after_isolated_stages(
        cache_root,
        source_path,
        &helper_path,
    )?)
}

fn reject_known_quarantined_raw_export(admission: NativeDecodeAdmission) -> AnyResult<()> {
    match admission {
        NativeDecodeAdmission::NotQuarantined => Ok(()),
        NativeDecodeAdmission::Quarantined { observation } => bail!(
            "RAW export is temporarily unavailable: Shadow quarantined this unchanged source after {}. Use its cached preview, or update the source/decoder helper before retrying full-quality export.",
            observation.diagnostic_label()
        ),
    }
}

#[cfg(test)]
mod tests;
