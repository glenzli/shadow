//! Real DesktopSession isolation across Catalog, attachments, cache and job checkpoints.

use std::{fs, io::Read as _, path::Path};

use shadow_ai::{
    ArtifactHashAlgorithm, GeneratedArtifactReference, ImageEmbeddingEvidence,
    InferRuntimeClientError, SemanticEmbeddingProvider, SemanticRequestPriority,
    TextEmbeddingEvidence,
};
use shadow_catalog::RegisterAsset;
use shadow_core::{
    FilesystemDerivedRasterStore, SmartCategoryDefinition, SmartClassificationPolicy,
    SmartClassificationRequest, classify_review_smart_categories, fingerprint_source,
};
use shadow_domain::RepresentationKind;

use crate::{
    DesktopSession, ffi, native_path_ffi, open_desktop_session_at,
    subject_mask_runtime::config::SubjectMaskRuntimePaths,
    tests::fixtures::grade_stack::ffi_parameters,
};

struct OfflineEmbeddingProvider;

impl SemanticEmbeddingProvider for OfflineEmbeddingProvider {
    fn embed_image_semantics(
        &self,
        _: &[u8],
        _: &str,
        _: &str,
        _: SemanticRequestPriority,
    ) -> Result<ImageEmbeddingEvidence, InferRuntimeClientError> {
        Err(InferRuntimeClientError::Input(
            "offline profile fixture".into(),
        ))
    }

    fn embed_text_semantics(
        &self,
        _: &str,
        _: &str,
        _: Option<&str>,
        _: SemanticRequestPriority,
    ) -> Result<TextEmbeddingEvidence, InferRuntimeClientError> {
        Err(InferRuntimeClientError::Input(
            "offline profile fixture".into(),
        ))
    }
}

fn open(root: &Path) -> Box<DesktopSession> {
    open_desktop_session_at(&root.join("catalog.sqlite"), &root.join("cache"))
        .expect("open independent desktop session")
}

fn register(session: &DesktopSession, source: &Path) -> String {
    let fingerprint = fingerprint_source(source).expect("fingerprint shared read-only fixture");
    session
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: shadow_core::native_location(source),
            byte_len: fingerprint.byte_len,
            modified_at_ms: fingerprint.modified_at_ms,
            now_ms: 100,
        })
        .expect("register original in selected Catalog")
        .photo_id
        .to_string()
}

fn failed_checkpoint(session: &DesktopSession, revision: &str) {
    assert!(
        classify_review_smart_categories(
            &session.catalog,
            session.cache_root.clone(),
            &OfflineEmbeddingProvider,
            &[SmartCategoryDefinition {
                id: "portrait".into(),
                description: "a portrait photograph".into(),
                minimum_similarity: 0.07,
            }],
            &SmartClassificationRequest {
                config_revision: revision.into(),
                generation: None,
                start_new: true,
                clear_embeddings: false,
            },
            SmartClassificationPolicy::default(),
        )
        .is_err()
    );
}

#[test]
fn two_profiles_keep_edits_attachments_cache_and_resumable_jobs_separate() {
    let fixture = tempfile::tempdir().expect("private profile fixture");
    let daily_root = fixture.path().join("daily");
    let debug_root = fixture.path().join("debug");
    let source = fixture.path().join("shared-original.dng");
    let source_bytes = [0_u8; 4096];
    fs::write(&source, source_bytes).expect("create synthetic original");
    let daily = open(&daily_root);
    let debug = open(&debug_root);
    let daily_photo = register(&daily, &source);
    let debug_photo = register(&debug, &source);
    let source_path = source.to_str().expect("fixture path");
    let daily_edit = daily
        .save_basic_edit_version_at(
            &daily_photo,
            source_path,
            "",
            &ffi_parameters(0.25, 1.1, [0.0; 2], 0.9),
            "Daily look",
            1000,
        )
        .expect("persist Daily edit");
    assert!(
        !debug
            .photo_edit_state(&debug_photo, source_path)
            .expect("Debug edit state")
            .has_working_version
    );
    let debug_edit = debug
        .save_basic_edit_version_at(
            &debug_photo,
            source_path,
            "",
            &ffi_parameters(0.75, 1.2, [0.0; 2], 0.8),
            "Debug look",
            2000,
        )
        .expect("persist Debug edit");
    assert_ne!(daily_edit.working_commit_id, debug_edit.working_commit_id);
    assert_eq!(
        daily
            .photo_edit_state(&daily_photo, source_path)
            .unwrap()
            .working_commit_id,
        daily_edit.working_commit_id
    );

    let daily_blob = daily
        .loader
        .store_bytes(b"daily preview")
        .expect("store Daily cache");
    assert!(
        shadow_cache::ContentAddressedStore::open(&debug.cache_root)
            .unwrap()
            .read_verified(daily_blob.digest)
            .is_err()
    );

    let daily_paths =
        SubjectMaskRuntimePaths::discover(&std::env::current_exe().unwrap(), &daily.cache_root)
            .unwrap();
    let debug_paths =
        SubjectMaskRuntimePaths::discover(&std::env::current_exe().unwrap(), &debug.cache_root)
            .unwrap();
    assert_eq!(
        daily_paths.derived_raster_store_root,
        daily_root.join("derived-rasters")
    );
    assert_eq!(
        debug_paths.derived_raster_store_root,
        debug_root.join("derived-rasters")
    );
    let daily_store =
        FilesystemDerivedRasterStore::open(daily_paths.derived_raster_store_root).unwrap();
    let debug_store =
        FilesystemDerivedRasterStore::open(debug_paths.derived_raster_store_root).unwrap();
    let mask_bytes = [0_u8, 32, 128, 255];
    let provider_output = fixture.path().join("synthetic-mask");
    fs::write(&provider_output, mask_bytes).unwrap();
    let mask = GeneratedArtifactReference::new(
        ArtifactHashAlgorithm::Blake3_256,
        blake3::hash(&mask_bytes).to_hex().to_string(),
        4,
        "application/x-shadow-soft-mask".into(),
        1,
    )
    .unwrap();
    daily_store
        .stage_proposal_file(&mask, &provider_output)
        .unwrap();
    assert!(debug_store.open_staged_proposal(&mask).is_err());
    let mut actual_mask = Vec::new();
    daily_store
        .open_staged_proposal(&mask)
        .unwrap()
        .read_to_end(&mut actual_mask)
        .unwrap();
    assert_eq!(actual_mask, mask_bytes);

    let export = daily
        .enqueue_durable_export_job(
            vec![ffi::FfiDurableExportTarget {
                photo_id: daily_photo.clone(),
                source_path: source_path.into(),
                recipe_commit_id: String::new(),
                representation_id: String::new(),
                output_path: native_path_ffi::location_to_ffi(&shadow_core::native_location(
                    &daily_root.join("export.png"),
                ))
                .unwrap(),
            }],
            r#"{"format":"png"}"#,
        )
        .expect("queue Daily export");
    assert!(!export.job_id.is_empty());
    assert!(!debug.claim_next_durable_export_item().unwrap().has_item);
    failed_checkpoint(&daily, "daily-taxonomy");
    let daily_job = daily.smart_classification_snapshot().unwrap();
    assert_eq!(daily_job.status, "failed");
    assert_eq!(
        debug.smart_classification_snapshot().unwrap().status,
        "empty"
    );
    failed_checkpoint(&debug, "debug-taxonomy");
    let debug_job = debug.smart_classification_snapshot().unwrap();
    assert_ne!(daily_job.generation, debug_job.generation);
    debug
        .pause_smart_classification(&debug_job.generation)
        .unwrap();
    assert_eq!(
        daily.smart_classification_snapshot().unwrap().status,
        "failed"
    );
    assert_eq!(
        debug.smart_classification_snapshot().unwrap().status,
        "paused"
    );
    assert_eq!(fs::read(&source).unwrap(), source_bytes);
    drop(daily);
    drop(debug);

    let daily = open(&daily_root);
    let debug = open(&debug_root);
    assert_eq!(
        daily
            .photo_edit_state(&daily_photo, source_path)
            .unwrap()
            .working_commit_id,
        daily_edit.working_commit_id
    );
    assert_eq!(
        debug
            .photo_edit_state(&debug_photo, source_path)
            .unwrap()
            .working_commit_id,
        debug_edit.working_commit_id
    );
    assert_eq!(
        daily.smart_classification_snapshot().unwrap().generation,
        daily_job.generation
    );
    assert_eq!(
        debug.smart_classification_snapshot().unwrap().generation,
        debug_job.generation
    );
    assert!(!debug.claim_next_durable_export_item().unwrap().has_item);
    assert_eq!(
        daily.claim_next_durable_export_item().unwrap().job_id,
        export.job_id
    );
    assert_eq!(fs::read(&source).unwrap(), source_bytes);
}
