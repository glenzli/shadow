mod feedback_contract_support;

use feedback_contract_support::{presentation, register_source};
use shadow_ai::{
    FeedbackAction, LearningScope, NewFeedbackEvent, PresentedCandidate, PresentedFitMode,
    PresentedVisualArtifact, PresentedVisualFrame, PresentedVisualProvenance, PresentedVisualRole,
    UnitInterval,
};
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, Catalog, InvalidateCachedArtifactStatus,
    RecordCachedArtifact, RecordCachedArtifactStatus, RepresentationFingerprint,
};
use shadow_domain::{ImageDimensions, PreviewByteOrder, PreviewCodec};

fn presented_visual(
    representation_id: shadow_domain::RepresentationId,
) -> PresentedVisualProvenance {
    PresentedVisualProvenance {
        artifact: PresentedVisualArtifact {
            representation_id,
            source_byte_len: 42,
            source_modified_at_ms: Some(1),
            role: PresentedVisualRole::EmbeddedPreview,
            variant_key: "embedded-0".into(),
            generator_id: "test-preview-extractor".into(),
            generator_version: "1".into(),
            provider_preview_id: Some(0),
            blob_algorithm: "blake3".into(),
            blob_digest_hex: "07".repeat(32),
            blob_byte_len: 2_048,
            codec: "jpeg".into(),
            byte_order: "not_applicable".into(),
            width: 1_920,
            height: 1_280,
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 1_700_000_000_100,
        },
        frame: PresentedVisualFrame {
            surface_id: "review-compare-left".into(),
            surface_revision: 1,
            fit_mode: PresentedFitMode::PreserveAspectFit,
            decoder_id: "qt-image-jpeg".into(),
            decoder_version: "6.8.3".into(),
            auto_transform: true,
            requested_width: 960,
            requested_height: 640,
            decoded_width: 1_920,
            decoded_height: 1_280,
            pixel_format: "rgba8888-premultiplied".into(),
            pixel_hash_algorithm: "blake3".into(),
            pixel_hash_hex: "09".repeat(32),
        },
    }
}

#[test]
fn presented_visual_history_survives_rebuildable_cache_deletion() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let registered = register_source(&mut catalog, 1);
    let source = RepresentationFingerprint {
        byte_len: 42,
        modified_at_ms: Some(1),
    };
    assert_eq!(
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: source,
                artifact: CachedArtifact {
                    role: CachedArtifactRole::EmbeddedPreview,
                    variant_key: "embedded-0".into(),
                    generator_id: "test-preview-extractor".into(),
                    generator_version: "1".into(),
                    recipe_snapshot_digest: None,
                    provider_preview_id: Some(0),
                    blob_algorithm: "blake3".into(),
                    blob_digest: [7; 32],
                    blob_byte_len: 2_048,
                    codec: PreviewCodec::Jpeg,
                    byte_order: PreviewByteOrder::NotApplicable,
                    dimensions: ImageDimensions {
                        width: 1_920,
                        height: 1_280,
                    },
                    bits_per_channel: 8,
                    channels: 3,
                    created_at_ms: 1_700_000_000_100,
                },
            })
            .expect("record cache artifact"),
        RecordCachedArtifactStatus::Recorded
    );
    let visual = presented_visual(registered.representation_id);
    let event = NewFeedbackEvent {
        event_id: "visual-survives-cache".into(),
        occurred_at_unix_ms: 1_700_000_001_000,
        scope: LearningScope::Global,
        presentation: presentation(vec![PresentedCandidate {
            photo_id: registered.photo_id,
            position: 0,
            visible_fraction: UnitInterval::ONE,
            inspected_at_one_to_one: false,
            feature: None,
            visual: Some(visual.clone()),
        }]),
        action: FeedbackAction::Exported {
            photo_id: registered.photo_id,
        },
    };
    catalog
        .append_feedback_event(&event)
        .expect("append visual evidence");

    let cached = catalog
        .preferred_cached_artifact(registered.representation_id)
        .expect("load preferred cache artifact")
        .expect("cached artifact");
    assert_eq!(
        catalog
            .invalidate_cached_artifact(&cached)
            .expect("invalidate cache artifact"),
        InvalidateCachedArtifactStatus::Invalidated
    );
    assert!(
        catalog
            .cached_artifacts(registered.representation_id)
            .expect("read deleted cache rows")
            .is_empty()
    );

    let page = catalog
        .feedback_events_after(&LearningScope::Global, 0, 10)
        .expect("read historical evidence");
    assert_eq!(
        page.events[0].presentation.candidates[0].visual,
        Some(visual)
    );
}
