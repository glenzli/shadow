use shadow_ai::{
    FeatureSnapshotRef, FeedbackAction, LearningScope, NewFeedbackEvent, PairwiseOutcome,
    PresentationContext, PresentedCandidate, PresentedFitMode, PresentedVisualArtifact,
    PresentedVisualFrame, PresentedVisualProvenance, PresentedVisualRole, UnitInterval,
};
use shadow_domain::{AssetLocation, PhotoId, Platform, RepresentationId, RepresentationKind};

use super::super::*;
use crate::{RegisterAsset, RegisteredAsset, RegistrationStatus};

pub(super) fn register_source(catalog: &mut Catalog, index: u32) -> RegisteredAsset {
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                format!("/photos/feedback-{index}.dng").into_bytes(),
                format!("/photos/feedback-{index}.dng"),
            ),
            byte_len: 42,
            modified_at_ms: Some(i64::from(index)),
            now_ms: 1_700_000_000_000 + i64::from(index),
        })
        .expect("register feedback photo");
    assert_eq!(registered.status, RegistrationStatus::Inserted);
    registered
}

pub(super) fn register_photo(catalog: &mut Catalog, index: u32) -> PhotoId {
    register_source(catalog, index).photo_id
}

pub(super) fn presentation(candidates: Vec<PresentedCandidate>) -> PresentationContext {
    PresentationContext {
        session_id: "review-session".into(),
        group_id: None,
        candidates,
        active_model: None,
    }
}

pub(super) fn candidate(photo_id: PhotoId, position: u32, hash: &str) -> PresentedCandidate {
    PresentedCandidate {
        photo_id,
        position,
        visible_fraction: UnitInterval::ONE,
        inspected_at_one_to_one: false,
        feature: Some(FeatureSnapshotRef {
            extractor_id: "frozen-features".into(),
            extractor_revision: "r1".into(),
            preprocessing_version: "display-proxy-v1".into(),
            artifact_hash: hash.into(),
            dimension: 3,
        }),
        visual: None,
    }
}

pub(super) fn presented_visual(representation_id: RepresentationId) -> PresentedVisualProvenance {
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

pub(super) fn exported_event(
    event_id: &str,
    scope: LearningScope,
    photo_id: PhotoId,
) -> NewFeedbackEvent {
    NewFeedbackEvent {
        event_id: event_id.into(),
        occurred_at_unix_ms: 1_700_000_001_000,
        scope,
        presentation: presentation(vec![]),
        action: FeedbackAction::Exported { photo_id },
    }
}

pub(super) fn pairwise_event(
    event_id: &str,
    scope: LearningScope,
    left: PhotoId,
    right: PhotoId,
) -> NewFeedbackEvent {
    NewFeedbackEvent {
        event_id: event_id.into(),
        occurred_at_unix_ms: 1_700_000_001_000,
        scope,
        presentation: presentation(vec![
            candidate(left, 0, "left-feature"),
            candidate(right, 1, "right-feature"),
        ]),
        action: FeedbackAction::PairwiseComparison {
            left,
            right,
            outcome: PairwiseOutcome::LeftPreferred,
        },
    }
}

pub(super) fn catalog_with_exported_event(event_id: &str) -> Catalog {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let photo = register_photo(&mut catalog, 1);
    catalog
        .append_feedback_event(&exported_event(event_id, LearningScope::Global, photo))
        .expect("append exported event");
    catalog
}
