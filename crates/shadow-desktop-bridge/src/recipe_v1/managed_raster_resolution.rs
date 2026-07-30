//! Verified application-store resolution for executable managed masks.
//!
//! Recipe compilation remains a pure graph projection. This owner supplies the
//! one application-service capability it needs for a managed raster: bounded,
//! digest-verified bytes from durable storage beside (never inside) the
//! rebuildable runtime cache.

use std::{
    io::Read,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result as AnyResult, bail};
use shadow_bridge::{
    AdjustmentLocalMask, AdjustmentRasterMaskEncoding, MAX_MANAGED_RASTER_MASK_BYTES,
};
use shadow_core::FilesystemDerivedRasterStore;
use shadow_domain::{ManagedRasterMask, RasterMaskEncoding};

const DERIVED_RASTER_STORE_DIRECTORY: &str = "derived-rasters";

pub(crate) trait ManagedRasterMaskResolver {
    fn resolve(
        &self,
        raster: &ManagedRasterMask,
        expansion: f64,
        feather: f64,
        invert: bool,
    ) -> AnyResult<AdjustmentLocalMask>;
}

#[derive(Debug)]
pub(super) struct FilesystemManagedRasterMaskResolver {
    store: FilesystemDerivedRasterStore,
}

impl FilesystemManagedRasterMaskResolver {
    pub(super) fn open_for_runtime_cache(runtime_cache_root: &Path) -> AnyResult<Self> {
        let store_root = managed_raster_store_root(runtime_cache_root)?;
        Ok(Self {
            store: FilesystemDerivedRasterStore::open(&store_root).with_context(|| {
                format!(
                    "open durable managed raster store beside {}",
                    runtime_cache_root.display()
                )
            })?,
        })
    }
}

impl ManagedRasterMaskResolver for FilesystemManagedRasterMaskResolver {
    fn resolve(
        &self,
        raster: &ManagedRasterMask,
        expansion: f64,
        feather: f64,
        invert: bool,
    ) -> AnyResult<AdjustmentLocalMask> {
        let byte_len = usize::try_from(raster.byte_len())
            .context("managed raster mask byte length exceeds the host address space")?;
        if byte_len > MAX_MANAGED_RASTER_MASK_BYTES {
            bail!(
                "managed raster mask contains {} bytes, but native execution accepts at most {}",
                raster.byte_len(),
                MAX_MANAGED_RASTER_MASK_BYTES
            );
        }
        let file = self
            .store
            .open_recipe_mask(raster)
            .context("verify managed raster mask object")?;
        let read_limit = raster
            .byte_len()
            .checked_add(1)
            .context("managed raster mask read limit overflow")?;
        let mut samples = Vec::with_capacity(byte_len);
        file.take(read_limit)
            .read_to_end(&mut samples)
            .context("read verified managed raster mask object")?;
        if samples.len() != byte_len {
            bail!(
                "verified managed raster mask changed while reading: expected {byte_len} bytes, received {}",
                samples.len()
            );
        }
        let encoding = match raster.encoding() {
            RasterMaskEncoding::Gray8Unorm => AdjustmentRasterMaskEncoding::Gray8,
            RasterMaskEncoding::Gray16Float => AdjustmentRasterMaskEncoding::Gray16Float,
        };
        Ok(AdjustmentLocalMask::ManagedRaster {
            raster_width: raster.raster_width(),
            raster_height: raster.raster_height(),
            coordinate_width: raster.coordinate_width(),
            coordinate_height: raster.coordinate_height(),
            encoding,
            samples,
            expansion,
            feather,
            invert,
        })
    }
}

fn managed_raster_store_root(runtime_cache_root: &Path) -> AnyResult<PathBuf> {
    let application_root = runtime_cache_root.parent().with_context(|| {
        format!(
            "runtime cache root {} has no application-data parent",
            runtime_cache_root.display()
        )
    })?;
    Ok(application_root.join(DERIVED_RASTER_STORE_DIRECTORY))
}

#[cfg(test)]
mod tests;
