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

#[test]
fn restored_profile_resolves_lut_bytes_without_changing_the_immutable_recipe() {
    use crate::recipe_lut_resources::{
        collect_lut_resources, local_lut_path, read_verified_lut, relocate_draft_luts,
    };
    use crate::recipe_v1::compile_recipe_render_plan_with_managed_rasters;
    use crate::recipe_v1::{GradeStackDraft, LutEditParameters, grade_stack_recipe_v1_snapshot};
    use sha2::{Digest, Sha256};
    use shadow_bridge::AdjustmentRenderOperation;
    use std::fs;

    let source = tempfile::tempdir().unwrap();
    let restored = tempfile::tempdir().unwrap();
    let text = "TITLE \"Portable LUT\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n".to_owned();
    let id = format!("{:x}", Sha256::digest(text.as_bytes()));
    let old = local_lut_path(source.path(), &id);
    let local = local_lut_path(restored.path(), &id);
    fs::create_dir_all(old.parent().unwrap()).unwrap();
    fs::create_dir_all(local.parent().unwrap()).unwrap();
    fs::write(&old, &text).unwrap();
    fs::write(&local, &text).unwrap();
    let mut draft = GradeStackDraft::default();
    draft.grade_nodes[0].fine.lut = LutEditParameters {
        resource_id: id.clone(),
        title: "Preserved LUT".into(),
        managed_path: old.to_str().unwrap().into(),
        intensity: 0.37,
    };
    let snapshot = grade_stack_recipe_v1_snapshot(&draft, None).unwrap();
    let immutable = serde_json::to_vec(&snapshot).unwrap();
    fs::remove_file(&old).unwrap();
    let resolver =
        FilesystemManagedRasterMaskResolver::open_for_runtime_cache(&restored.path().join("cache"))
            .unwrap();
    let plan = compile_recipe_render_plan_with_managed_rasters(&snapshot, &resolver).unwrap();
    assert!(plan.nodes.iter().any(|node|matches!(&node.operation,AdjustmentRenderOperation::Lut3D{document,intensity} if document==text.as_bytes() && *intensity==0.37)));
    assert_eq!(serde_json::to_vec(&snapshot).unwrap(), immutable);
    relocate_draft_luts(&mut draft, restored.path()).unwrap();
    assert_eq!(
        draft.grade_nodes[0].fine.lut.managed_path,
        local.to_str().unwrap()
    );
    assert_eq!(collect_lut_resources(&draft).unwrap()[0].document(), text);
    fs::write(&local, b"wrong local bytes").unwrap();
    assert!(compile_recipe_render_plan_with_managed_rasters(&snapshot, &resolver).is_err());
    // A surviving old library cannot mask corruption in the new one.
    fs::write(&old, &text).unwrap();
    assert!(read_verified_lut(&old, &id, Some(restored.path())).is_err());
}
