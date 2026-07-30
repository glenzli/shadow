use std::path::Path;

use shadow_bridge::{AdjustmentLocalMask, AdjustmentRasterMaskEncoding};
use shadow_domain::{EntityId, ManagedRasterMask, RasterMaskEncoding};

use super::{
    FilesystemManagedRasterMaskResolver, ManagedRasterMaskResolver, managed_raster_store_root,
};

fn fixture_root(label: &str) -> std::path::PathBuf {
    std::env::temp_dir().join(format!(
        "shadow-managed-raster-resolution-{label}-{}-{}",
        std::process::id(),
        shadow_domain::RecipeCommitId::new_v7()
    ))
}

fn write_managed_mask(
    cache_root: &Path,
    samples: &[u8],
    encoding: RasterMaskEncoding,
) -> ManagedRasterMask {
    let digest = blake3::hash(samples).to_hex().to_string();
    let object_id = format!("objects/v1/b3/{}/{}", &digest[..2], &digest[2..]);
    let object_path = managed_raster_store_root(cache_root)
        .expect("derive durable store")
        .join(&object_id);
    std::fs::create_dir_all(object_path.parent().expect("object parent"))
        .expect("create object directory");
    std::fs::write(&object_path, samples).expect("write managed object");
    ManagedRasterMask::new(
        object_id,
        1,
        digest,
        u64::try_from(samples.len()).expect("fixture byte length"),
        2,
        2,
        6_000,
        4_000,
        encoding,
    )
    .expect("managed mask reference")
}

#[test]
fn resolver_reads_verified_bytes_from_a_store_beside_the_cache() {
    let root = fixture_root("valid");
    let cache_root = root.join("cache");
    let samples = [0_u8, 64, 128, 255];
    let raster = write_managed_mask(&cache_root, &samples, RasterMaskEncoding::Gray8Unorm);
    let resolver = FilesystemManagedRasterMaskResolver::open_for_runtime_cache(&cache_root)
        .expect("open resolver");

    assert_eq!(
        resolver
            .resolve(&raster, -0.35, 0.24, true)
            .expect("resolve mask"),
        AdjustmentLocalMask::ManagedRaster {
            raster_width: 2,
            raster_height: 2,
            coordinate_width: 6_000,
            coordinate_height: 4_000,
            encoding: AdjustmentRasterMaskEncoding::Gray8,
            samples: samples.to_vec(),
            expansion: -0.35,
            feather: 0.24,
            invert: true,
        }
    );
    assert!(
        managed_raster_store_root(&cache_root)
            .expect("store root")
            .starts_with(&root)
    );
    assert!(
        !managed_raster_store_root(&cache_root)
            .expect("store root")
            .starts_with(&cache_root),
        "durable Recipe authority must not live below the rebuildable cache"
    );

    std::fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn resolver_rehashes_the_object_and_fails_closed_after_tampering() {
    let root = fixture_root("tampered");
    let cache_root = root.join("cache");
    let raster = write_managed_mask(
        &cache_root,
        &[0x00, 0x00, 0x00, 0x38, 0x00, 0x3c, 0x00, 0x34],
        RasterMaskEncoding::Gray16Float,
    );
    let object_path = managed_raster_store_root(&cache_root)
        .expect("store root")
        .join(raster.store_object_id());
    std::fs::write(object_path, [0_u8; 8]).expect("tamper object");
    let resolver = FilesystemManagedRasterMaskResolver::open_for_runtime_cache(&cache_root)
        .expect("open resolver");

    let error = resolver
        .resolve(&raster, 0.0, 0.0, false)
        .expect_err("substituted managed bytes must fail closed");
    assert!(format!("{error:#}").contains("digest"));

    std::fs::remove_dir_all(root).expect("remove fixture");
}
