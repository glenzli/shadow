use shadow_domain::{EntityId, PreviewDescriptorSnapshot};

use super::*;

fn publication(
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
) -> EmbeddedPreviewPublication {
    EmbeddedPreviewPublication {
        representation_id,
        source,
        preview: shadow_domain::PreviewPayload {
            descriptor: PreviewDescriptorSnapshot {
                provider_id: 7,
                codec: PreviewCodec::Jpeg,
                dimensions: ImageDimensions {
                    width: 12,
                    height: 8,
                },
                bits_per_channel: 8,
                channels: 3,
                encoded_bytes: 4,
                decodable: true,
            },
            byte_order: PreviewByteOrder::NotApplicable,
            bytes: vec![1, 2, 3, 4],
        },
    }
}

#[test]
fn accepts_only_the_current_source_revision() {
    let store = SessionPreviewStore::default();
    let representation_id = RepresentationId::new_v7();
    let source = RepresentationFingerprint {
        byte_len: 12,
        modified_at_ms: Some(4),
    };
    store
        .publish_embedded_preview(publication(representation_id, source))
        .expect("publish session preview");

    let descriptor = store
        .lookup(representation_id, source)
        .expect("lookup matching preview");
    assert_eq!(
        store.load(descriptor).as_deref(),
        Some([1_u8, 2, 3, 4].as_slice())
    );
    assert!(
        store
            .lookup(
                representation_id,
                RepresentationFingerprint {
                    byte_len: 13,
                    modified_at_ms: Some(4),
                },
            )
            .is_none()
    );
    assert_eq!(store.retained_bytes(), 4);
}
