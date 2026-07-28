use std::fs;

use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogActor, RecordCachedArtifact,
    RepresentationFingerprint,
};
use shadow_domain::{
    AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec,
    RepresentationKind,
};

use super::*;

#[test]
fn corrupt_blob_is_quarantined_and_exact_catalog_reference_is_invalidated() {
    let fixture = Fixture::new();
    let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
    let catalog = actor.handle();
    let registered = catalog
        .register_asset(&shadow_catalog::RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                fixture.raw_path.as_os_str().as_encoded_bytes().to_vec(),
                fixture.raw_path.display().to_string(),
            ),
            byte_len: fixture.source.byte_len,
            modified_at_ms: fixture.source.modified_at_ms,
            now_ms: 100,
        })
        .expect("register source");
    let store = ContentAddressedStore::open(&fixture.cache_root).expect("open cache");
    let blob = store.put(b"proxy jpeg").expect("store proxy");
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: registered.representation_id,
            expected_source: fixture.source,
            artifact: artifact(blob.digest, blob.byte_len),
        })
        .expect("record artifact");
    let record = catalog
        .cached_artifacts(registered.representation_id)
        .expect("read artifact")
        .remove(0);
    let loader =
        CachedArtifactLoader::open(catalog.clone(), &fixture.cache_root).expect("open loader");
    assert_eq!(
        loader.load_bytes(&record).expect("load proxy"),
        b"proxy jpeg"
    );

    fs::write(store.resolve(blob.digest), b"corrupt jpeg").expect("corrupt proxy");
    assert!(matches!(
        loader.load_bytes(&record),
        Err(CachedArtifactLoadError::Invalidated {
            reason: CachedArtifactInvalidationReason::DigestMismatch
        })
    ));
    assert!(
        catalog
            .cached_artifacts(registered.representation_id)
            .expect("read invalidated artifacts")
            .is_empty()
    );
    assert!(!store.resolve(blob.digest).exists());
    assert!(
        fixture
            .cache_root
            .join("quarantine")
            .join("b3")
            .read_dir()
            .expect("read quarantine")
            .next()
            .is_some()
    );

    actor.shutdown().expect("shutdown catalog");
}

fn artifact(digest: BlobDigest, byte_len: u64) -> CachedArtifact {
    CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: "test:grid-jpeg-2048-q88-v1".into(),
        generator_id: "test".into(),
        generator_version: "1".into(),
        recipe_snapshot_digest: None,
        provider_preview_id: None,
        blob_algorithm: digest.algorithm().into(),
        blob_digest: *digest.as_bytes(),
        blob_byte_len: byte_len,
        codec: PreviewCodec::Jpeg,
        byte_order: PreviewByteOrder::NotApplicable,
        dimensions: ImageDimensions {
            width: 2_048,
            height: 1_365,
        },
        bits_per_channel: 8,
        channels: 3,
        created_at_ms: 456,
    }
}

struct Fixture {
    root: PathBuf,
    raw_path: PathBuf,
    database_path: PathBuf,
    cache_root: PathBuf,
    source: RepresentationFingerprint,
}

impl Fixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!(
            "shadow-artifact-loader-{}-{}",
            std::process::id(),
            shadow_domain::RepresentationId::new_v7()
        ));
        fs::create_dir_all(&root).expect("create fixture");
        let raw_path = root.join("input.dng");
        fs::write(&raw_path, b"raw fixture").expect("write RAW fixture");
        let metadata = raw_path.metadata().expect("read RAW metadata");
        Self {
            database_path: root.join("catalog.sqlite"),
            cache_root: root.join("cache"),
            source: RepresentationFingerprint {
                byte_len: metadata.len(),
                modified_at_ms: Some(123),
            },
            raw_path,
            root,
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.root).expect("remove fixture");
    }
}
