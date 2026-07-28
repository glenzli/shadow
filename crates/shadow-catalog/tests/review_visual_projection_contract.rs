use shadow_catalog::{
    CachedArtifact, CachedArtifactGeneratorIdentity, CachedArtifactRole, Catalog, CommitRecipe,
    RecipeRefKind, RecipeRefTarget, RecordCachedArtifact, RegisterAsset, RepresentationFingerprint,
    TechnicalObservationRevision,
};
use shadow_domain::{
    AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec,
    RecipeCommit, RecipeId, RecipeSnapshot, RepresentationKind,
};

#[test]
fn review_page_treats_an_old_recipe_preview_generator_as_a_cache_miss() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let source = RepresentationFingerprint {
        byte_len: 4_096,
        modified_at_ms: Some(123),
    };
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/generator-aware.dng".to_vec(),
                "/photos/generator-aware.dng",
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 100,
        })
        .expect("register RAW");
    let generated_proxy = CachedArtifact {
        role: CachedArtifactRole::GeneratedProxy,
        variant_key: "proxy-v1".into(),
        generator_id: "libraw".into(),
        generator_version: "1".into(),
        recipe_snapshot_digest: None,
        provider_preview_id: None,
        blob_algorithm: "blake3-256".into(),
        blob_digest: [1; 32],
        blob_byte_len: 1_024,
        codec: PreviewCodec::Jpeg,
        byte_order: PreviewByteOrder::NotApplicable,
        dimensions: ImageDimensions {
            width: 1_600,
            height: 1_200,
        },
        bits_per_channel: 8,
        channels: 3,
        created_at_ms: 150,
    };
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: registered.representation_id,
            expected_source: source,
            artifact: generated_proxy.clone(),
        })
        .expect("record generated fallback");

    let recipe = RecipeCommit::new(
        shadow_domain::RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        RecipeSnapshot::empty(),
        Some("working edit".into()),
        200,
    )
    .expect("build working Recipe");
    let recipe = catalog
        .commit_recipe(&CommitRecipe {
            photo_id: registered.photo_id,
            commit: recipe,
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: None,
            }],
        })
        .expect("persist working Recipe");
    let stale_preview = CachedArtifact {
        role: CachedArtifactRole::RecipePreview,
        variant_key: "recipe-old-environment".into(),
        generator_id: "shadow-edit-preview".into(),
        generator_version: "shadow-edit-preview-v1;environment=old".into(),
        recipe_snapshot_digest: Some(recipe.snapshot_digest),
        blob_digest: [2; 32],
        dimensions: ImageDimensions {
            width: 2_400,
            height: 1_800,
        },
        created_at_ms: 250,
        ..generated_proxy.clone()
    };
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: registered.representation_id,
            expected_source: source,
            artifact: stale_preview.clone(),
        })
        .expect("record old-environment Recipe preview");

    let revision = TechnicalObservationRevision::current("generator-aware-review");
    let current_generator = CachedArtifactGeneratorIdentity {
        generator_id: "shadow-edit-preview".into(),
        generator_version: "shadow-edit-preview-v1;environment=current".into(),
    };
    let page = catalog
        .review_page_with_technical_and_recipe_preview_generator(
            None,
            10,
            &revision,
            &current_generator,
        )
        .expect("read generator-aware Review page");
    assert!(page.items[0].has_development_edits);
    assert_eq!(
        page.items[0]
            .visual
            .as_ref()
            .expect("fallback visual")
            .artifact,
        generated_proxy
    );
    assert!(
        catalog
            .cached_artifacts(registered.representation_id)
            .expect("list retained cache rows")
            .iter()
            .any(|record| record.artifact == stale_preview)
    );

    let current_preview = CachedArtifact {
        variant_key: "recipe-current-environment".into(),
        generator_version: current_generator.generator_version.clone(),
        blob_digest: [3; 32],
        dimensions: ImageDimensions {
            width: 1_200,
            height: 900,
        },
        created_at_ms: 300,
        ..stale_preview
    };
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: registered.representation_id,
            expected_source: source,
            artifact: current_preview.clone(),
        })
        .expect("record current-environment Recipe preview");
    let refreshed = catalog
        .review_page_with_technical_and_recipe_preview_generator(
            None,
            10,
            &revision,
            &current_generator,
        )
        .expect("read refreshed Review page");
    assert_eq!(
        refreshed.items[0]
            .visual
            .as_ref()
            .expect("current Recipe preview")
            .artifact,
        current_preview
    );
}

#[test]
fn review_query_returns_one_source_with_preferred_current_visual() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let source = RepresentationFingerprint {
        byte_len: 4_096,
        modified_at_ms: Some(123),
    };
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/input.dng".to_vec(),
                "/photos/input.dng",
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 100,
        })
        .expect("register RAW");
    for (role, key, digest, dimensions) in [
        (
            CachedArtifactRole::GeneratedProxy,
            "proxy-v1",
            [1; 32],
            ImageDimensions {
                width: 4_000,
                height: 3_000,
            },
        ),
        (
            CachedArtifactRole::EmbeddedPreview,
            "z-small",
            [2; 32],
            ImageDimensions {
                width: 1_600,
                height: 1_200,
            },
        ),
        (
            CachedArtifactRole::EmbeddedPreview,
            "b-large",
            [3; 32],
            ImageDimensions {
                width: 2_000,
                height: 1_000,
            },
        ),
        (
            CachedArtifactRole::EmbeddedPreview,
            "a-large",
            [4; 32],
            ImageDimensions {
                width: 2_000,
                height: 1_000,
            },
        ),
    ] {
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: source,
                artifact: CachedArtifact {
                    role,
                    variant_key: key.into(),
                    generator_id: "libraw".into(),
                    generator_version: "1".into(),
                    recipe_snapshot_digest: None,
                    provider_preview_id: None,
                    blob_algorithm: "blake3-256".into(),
                    blob_digest: digest,
                    blob_byte_len: 1_024,
                    codec: PreviewCodec::Jpeg,
                    byte_order: PreviewByteOrder::NotApplicable,
                    dimensions,
                    bits_per_channel: 8,
                    channels: 3,
                    created_at_ms: 456,
                },
            })
            .expect("record visual");
    }

    let preferred = catalog
        .preferred_cached_artifact(registered.representation_id)
        .expect("select shared preferred visual")
        .expect("preferred visual exists");
    let page = catalog.review_page(None, 128).expect("query Review page");
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.total_items, 1);
    assert!(page.next_cursor.is_none());
    assert_eq!(page.items[0].photo_id, registered.photo_id);
    assert_eq!(page.items[0].location.display_path, "/photos/input.dng");
    assert_eq!(page.items[0].visual.as_ref(), Some(&preferred));
    assert_eq!(
        page.items[0]
            .visual
            .as_ref()
            .expect("preferred visual")
            .artifact
            .role,
        CachedArtifactRole::GeneratedProxy
    );
    assert_eq!(preferred.artifact.variant_key, "proxy-v1");
}
