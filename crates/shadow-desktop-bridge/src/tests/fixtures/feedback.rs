//! Feedback candidates, catalog evidence, and incremental-training fixtures.

use std::{
    collections::BTreeSet,
    path::{Path, PathBuf},
};

use shadow_ai::{IncrementalTrainingPolicy, LearningScope, build_incremental_preference_batch};
use shadow_cache::ContentAddressedStore;
use shadow_catalog::{
    CachedArtifact, CachedArtifactRecord, CachedArtifactRole, RecordCachedArtifact, RegisterAsset,
    RepresentationFingerprint,
};
use shadow_domain::{
    AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec,
    RepresentationId, RepresentationKind,
};

use crate::{DesktopSession, open_desktop_session, review_service::ReviewVisualSelection};

#[derive(Debug, Clone)]
pub(in crate::tests) struct TestFeedbackCandidate {
    pub(in crate::tests) photo_id: String,
    pub(in crate::tests) representation_id: String,
    pub(in crate::tests) visual_handle: String,
    pub(in crate::tests) bytes: Vec<u8>,
    pub(in crate::tests) record: CachedArtifactRecord,
}

pub(in crate::tests) fn training_report(
    events: &[shadow_ai::FeedbackEvent],
    forgotten_event_ids: BTreeSet<String>,
) -> shadow_ai::BatchBuildReport {
    build_incremental_preference_batch(
        events,
        &IncrementalTrainingPolicy {
            scope: LearningScope::Global,
            learning_paused: false,
            after_sequence_exclusive: 0,
            maximum_examples: 10,
            forgotten_event_ids,
        },
    )
}

pub(in crate::tests) fn test_feedback_session() -> (
    PathBuf,
    Box<DesktopSession>,
    TestFeedbackCandidate,
    TestFeedbackCandidate,
) {
    let root = std::env::temp_dir().join(format!(
        "shadow-desktop-feedback-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create feedback fixture");
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open feedback session");
    let left = register_feedback_candidate(&session, &root, 1);
    let right = register_feedback_candidate(&session, &root, 2);
    (root, session, left, right)
}

fn register_feedback_candidate(
    session: &DesktopSession,
    root: &Path,
    index: u8,
) -> TestFeedbackCandidate {
    let source = RepresentationFingerprint {
        byte_len: 4_096 + u64::from(index),
        modified_at_ms: Some(100 + i64::from(index)),
    };
    let source_path = root
        .join(format!("feedback-{index}.dng"))
        .to_str()
        .expect("source path")
        .to_owned();
    let registered = session
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                source_path.as_bytes().to_vec(),
                source_path,
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 1_000 + i64::from(index),
        })
        .expect("register feedback source");
    let bytes = test_visual_bytes(index);
    let store = ContentAddressedStore::open(root.join("cache")).expect("open fixture CAS");
    let blob = store.put(&bytes).expect("write fixture visual blob");
    let record = CachedArtifactRecord {
        representation_id: registered.representation_id,
        source,
        artifact: CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: "feedback-proxy-v1".into(),
            generator_id: "test".into(),
            generator_version: "1".into(),
            recipe_snapshot_digest: None,
            provider_preview_id: None,
            blob_algorithm: blob.digest.algorithm().into(),
            blob_digest: *blob.digest.as_bytes(),
            blob_byte_len: blob.byte_len,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 4,
                height: 3,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 2_000 + i64::from(index),
        },
    };
    session
        .catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: record.representation_id,
            expected_source: record.source,
            artifact: record.artifact.clone(),
        })
        .expect("record feedback visual");
    let visual_handle = session
        .review
        .encode_grid_visual_handle(&ReviewVisualSelection {
            photo_id: registered.photo_id,
            record: record.clone(),
        })
        .expect("sign feedback visual handle");
    TestFeedbackCandidate {
        photo_id: registered.photo_id.to_string(),
        representation_id: registered.representation_id.to_string(),
        visual_handle,
        bytes,
        record,
    }
}

pub(in crate::tests) fn replace_feedback_visual(
    session: &DesktopSession,
    candidate: &TestFeedbackCandidate,
    byte: u8,
) -> CachedArtifactRecord {
    let bytes = test_visual_bytes(byte);
    let store = ContentAddressedStore::open(&session.cache_root).expect("open fixture CAS");
    let blob = store.put(&bytes).expect("write replacement visual blob");
    let mut record = candidate.record.clone();
    record.artifact.blob_digest = *blob.digest.as_bytes();
    record.artifact.blob_byte_len = blob.byte_len;
    record.artifact.generator_version = "2".into();
    record.artifact.created_at_ms += 100;
    session
        .catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id: record.representation_id,
            expected_source: record.source,
            artifact: record.artifact.clone(),
        })
        .expect("replace preferred visual");
    record
}

fn test_visual_bytes(byte: u8) -> Vec<u8> {
    let mut bytes = vec![0xff, 0xd8];
    bytes.extend(std::iter::repeat_n(byte, 32));
    bytes.extend([0xff, 0xd9]);
    bytes
}
