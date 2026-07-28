use shadow_domain::EntityId;

use super::*;
use crate::feedback::test_support::event;

fn presented_visual(representation_id: RepresentationId) -> PresentedVisualProvenance {
    PresentedVisualProvenance {
        artifact: PresentedVisualArtifact {
            representation_id,
            source_byte_len: 48_000_000,
            source_modified_at_ms: Some(1_700_000_000_000),
            role: PresentedVisualRole::EmbeddedPreview,
            variant_key: "embedded-0".into(),
            generator_id: "libraw-preview-extractor".into(),
            generator_version: "0.21.4".into(),
            provider_preview_id: Some(0),
            blob_algorithm: "blake3".into(),
            blob_digest_hex: "0".repeat(64),
            blob_byte_len: 2_000_000,
            codec: "jpeg".into(),
            byte_order: "not_applicable".into(),
            width: 4_096,
            height: 2_731,
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
            requested_width: 1_280,
            requested_height: 960,
            decoded_width: 4_096,
            decoded_height: 2_731,
            pixel_format: "rgba8888-premultiplied".into(),
            pixel_hash_algorithm: "blake3".into(),
            pixel_hash_hex: "a".repeat(64),
        },
    }
}

#[test]
fn legacy_candidate_json_round_trips_byte_for_byte_without_visual_field() {
    let legacy = r#"{"photo_id":"018f0000-0000-7000-8000-000000000000","position":7,"visible_fraction":1.0,"inspected_at_one_to_one":false,"feature":null}"#;
    let candidate: PresentedCandidate =
        serde_json::from_str(legacy).expect("deserialize legacy candidate");

    assert!(candidate.visual.is_none());
    assert_eq!(
        serde_json::to_string(&candidate).expect("serialize legacy candidate"),
        legacy
    );
}

#[test]
fn presented_visual_round_trips_with_complete_artifact_and_frame_identity() {
    let photo_id = PhotoId::new_v7();
    let mut evidence = event(FeedbackAction::Exported { photo_id });
    evidence.presentation.candidates.push(PresentedCandidate {
        photo_id,
        position: 0,
        visible_fraction: UnitInterval::ONE,
        inspected_at_one_to_one: false,
        feature: None,
        visual: Some(presented_visual(RepresentationId::new_v7())),
    });
    evidence.validate().expect("valid visual provenance");

    let json = serde_json::to_string(&evidence).expect("serialize visual evidence");
    let decoded: FeedbackEvent = serde_json::from_str(&json).expect("deserialize visual evidence");
    assert_eq!(decoded, evidence);
    assert_eq!(
        serde_json::to_string(&decoded).expect("reserialize visual evidence"),
        json
    );
}

#[test]
fn presented_visual_rejects_tampered_or_incomplete_identity() {
    let photo_id = PhotoId::new_v7();
    let mut evidence = event(FeedbackAction::Exported { photo_id });
    evidence.presentation.candidates.push(PresentedCandidate {
        photo_id,
        position: 0,
        visible_fraction: UnitInterval::ONE,
        inspected_at_one_to_one: false,
        feature: None,
        visual: Some(presented_visual(RepresentationId::new_v7())),
    });

    let visual = evidence.presentation.candidates[0]
        .visual
        .as_mut()
        .expect("visual");
    visual.artifact.blob_digest_hex.replace_range(0..1, "A");
    assert!(matches!(
        evidence.validate(),
        Err(FeedbackValidationError::InvalidVisualDigest {
            field: "visual blob digest"
        })
    ));

    let visual = evidence.presentation.candidates[0]
        .visual
        .as_mut()
        .expect("visual");
    visual.artifact.blob_digest_hex = "0".repeat(64);
    visual.frame.decoded_width = 0;
    assert!(matches!(
        evidence.validate(),
        Err(FeedbackValidationError::ZeroVisualValue {
            field: "visual decoded width"
        })
    ));

    let visual = evidence.presentation.candidates[0]
        .visual
        .as_mut()
        .expect("visual");
    visual.frame.decoded_width = 4_096;
    visual.frame.decoder_id.clear();
    assert!(matches!(
        evidence.validate(),
        Err(FeedbackValidationError::EmptyString {
            field: "visual decoder id"
        })
    ));

    let visual = evidence.presentation.candidates[0]
        .visual
        .as_mut()
        .expect("visual");
    visual.frame.decoder_id = "qt-image-jpeg".into();
    visual.artifact.generator_version = "x".repeat(MAX_IDENTIFIER_LENGTH + 1);
    assert!(matches!(
        evidence.validate(),
        Err(FeedbackValidationError::StringTooLong {
            field: "visual generator version",
            maximum: MAX_IDENTIFIER_LENGTH
        })
    ));
}
