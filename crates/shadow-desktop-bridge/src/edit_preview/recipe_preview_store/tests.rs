use shadow_bridge::{
    EditPreviewBackend, RawCameraProfileStatus, RawDevelopmentPlan, RawPipelinePath,
};
use shadow_catalog::{CatalogActor, RegisterAsset};
use shadow_domain::{
    AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec,
    ProxyPayload, RawTemperatureTint, RawWhiteBalance, RepresentationId, RepresentationKind,
};
use std::time::{Duration, Instant};

use super::*;

#[test]
fn saturated_durable_cache_queue_never_blocks_preview_publication() {
    let (sender, _receiver) = mpsc::sync_channel(1);
    assert_eq!(
        try_enqueue_rebuildable_cache_job(&sender, 1_u8),
        RecipePreviewStoreEnqueue::Queued
    );

    let started = Instant::now();
    assert_eq!(
        try_enqueue_rebuildable_cache_job(&sender, 2_u8),
        RecipePreviewStoreEnqueue::Dropped
    );
    assert!(
        started.elapsed() < Duration::from_millis(50),
        "a saturated rebuildable-cache queue must return immediately"
    );
}

#[test]
#[allow(clippy::too_many_lines)]
fn publication_preserves_recipe_preview_storage_and_reader_identity_contract() {
    let root = std::env::temp_dir().join(format!(
        "shadow-recipe-preview-store-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create Recipe-preview store fixture");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("open fixture Catalog");
    let catalog = actor.handle();
    let source = RepresentationFingerprint {
        byte_len: 4_096,
        modified_at_ms: Some(1_234),
    };
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/fixture/source.dng".to_vec(),
                "/fixture/source.dng",
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 2_000,
        })
        .expect("register fixture source");
    let loader = CachedArtifactLoader::open(catalog.clone(), root.join("cache"))
        .expect("open fixture cache");
    let proxy = ProxyPayload {
        dimensions: ImageDimensions {
            width: 2_048,
            height: 1_365,
        },
        codec: PreviewCodec::Jpeg,
        bits_per_channel: 8,
        channels: 3,
        bytes: vec![0xff, 0xd8, 0x11, 0x22, 0xff, 0xd9],
    };
    let raw_development_plan =
        RawDevelopmentPlan::preview().with_white_balance(RawWhiteBalance::temperature_tint(
            RawTemperatureTint::new(6_800, -14).expect("fixture temperature/tint"),
        ));
    let raw_pipeline_receipt =
        raw_pipeline_receipt("fixture-raw-pipeline-v1", raw_development_plan);
    let edit_execution_receipt = edit_execution_receipt("fixture-edit-execution-v1");
    let recipe_snapshot_digest = [0xab; 32];
    let source_environment_cache_identity = "source-environment-v1-fixture";

    store_recipe_preview(
        &catalog,
        &loader,
        RecipePreviewStoreRequest {
            representation_id: registered.representation_id,
            expected_source: source,
            proxy: &proxy,
            recipe_snapshot_digest,
            max_edge: 2_048,
            jpeg_quality: 90,
            raw_development_plan,
            raw_pipeline_receipt: &raw_pipeline_receipt,
            edit_execution_receipt: &edit_execution_receipt,
            source_environment_cache_identity,
        },
    )
    .expect("publish Recipe preview");

    let records = catalog
        .cached_artifacts(registered.representation_id)
        .expect("read published Recipe preview");
    let [record] = records.as_slice() else {
        panic!("expected exactly one Recipe-preview record, got {records:?}");
    };
    let raw_pipeline = prepared_raw_pipeline_cache_identity(&raw_pipeline_receipt)
        .expect("fixture RAW pipeline identity");
    let edit_execution = prepared_edit_execution_cache_identity(&edit_execution_receipt)
        .expect("fixture edit execution identity");
    let raw_plan_identity = raw_development_plan_identity(raw_development_plan)
        .expect("fixture preview RAW-plan identity");
    assert_eq!(record.representation_id, registered.representation_id);
    assert_eq!(record.source, source);
    assert_eq!(record.artifact.role, CachedArtifactRole::RecipePreview);
    assert_eq!(
        record.artifact.variant_key,
        format!(
            "shadow-recipe-preview:jpeg-2048-q90-444-v1;{raw_plan_identity};\
             pipeline={};execution={};recipe={}",
            raw_pipeline.component(),
            edit_execution.component(),
            encode_hex(&recipe_snapshot_digest)
        )
    );
    assert_eq!(record.artifact.generator_id, EDIT_PREVIEW_GENERATOR_ID);
    assert_eq!(
        record.artifact.generator_version,
        edit_preview_generator_version(
            source_environment_cache_identity,
            &edit_preview_generator_implementation_identity(),
        )
    );
    assert_eq!(
        record.artifact.recipe_snapshot_digest,
        Some(recipe_snapshot_digest)
    );
    assert_eq!(record.artifact.provider_preview_id, None);
    assert_eq!(record.artifact.codec, PreviewCodec::Jpeg);
    assert_eq!(record.artifact.byte_order, PreviewByteOrder::NotApplicable);
    assert_eq!(record.artifact.dimensions, proxy.dimensions);
    assert_eq!(record.artifact.bits_per_channel, proxy.bits_per_channel);
    assert_eq!(record.artifact.channels, proxy.channels);
    assert_eq!(
        loader.load_bytes(record).expect("read stored preview blob"),
        proxy.bytes
    );

    drop(loader);
    drop(catalog);
    actor.shutdown().expect("stop fixture Catalog");
    std::fs::remove_dir_all(root).expect("remove Recipe-preview store fixture");
}

fn raw_pipeline_receipt(cache_identity: &str, plan: RawDevelopmentPlan) -> RawPipelineReceipt {
    RawPipelineReceipt {
        schema_version: RawPipelineReceipt::CURRENT_SCHEMA_VERSION,
        path: RawPipelinePath::ShadowRawFrame,
        cache_identity: cache_identity.to_owned(),
        pipeline_identity: "fixture-pipeline".to_owned(),
        source_provider_id: "fixture-provider".to_owned(),
        source_provider_version: "1".to_owned(),
        fallback_reason: None,
        raw_frame_schema_version: 1,
        raw_developer_version: 1,
        requested_plan: plan,
        effective_plan: plan,
        camera_profile_status: RawCameraProfileStatus::NoMatch,
        camera_profile_catalog_identity: "fixture-profile-catalog".to_owned(),
        camera_profile_identity: String::new(),
        camera_profile_name: String::new(),
        camera_profile_diagnostic: None,
        camera_profile_developer_version:
            RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION,
    }
}

fn edit_execution_receipt(cache_identity: &str) -> EditPreviewExecutionReceipt {
    EditPreviewExecutionReceipt {
        schema_version: 1,
        cache_identity: cache_identity.to_owned(),
        adjustment_backend: EditPreviewBackend::Cpu,
        adjustment_backend_version: 1,
        adjustment_execution_contract_version: 1,
        display_backend: EditPreviewBackend::Cpu,
        display_backend_version: 1,
        display_output_contract_version: 1,
        fused_pipeline: false,
        adjustment_fell_back: false,
        display_fell_back: false,
        diagnostic: None,
    }
}
