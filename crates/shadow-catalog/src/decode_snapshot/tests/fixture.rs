use shadow_domain::{
    AssetLocation, DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport,
    DecoderSnapshot, ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, Platform,
    PreviewCodec, PreviewDescriptorSnapshot, RawDevelopmentCapabilitySnapshot, RawMetadataSnapshot,
    RepresentationId, RepresentationKind,
};

use crate::{Catalog, RegisterAsset, RegistrationStatus};

use super::super::RepresentationFingerprint;

pub(super) fn registered_catalog() -> (Catalog, RepresentationId, RepresentationFingerprint) {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let source = RepresentationFingerprint {
        byte_len: 1_024,
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
        .expect("register asset");
    assert_eq!(registered.status, RegistrationStatus::Inserted);
    (catalog, registered.representation_id, source)
}

pub(super) fn snapshot(provider_id: &str, version: &str, preview_ids: &[usize]) -> DecoderSnapshot {
    DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: provider_id.into(),
            version: version.into(),
            dng_sdk: false,
            rawspeed: false,
            jpeg: true,
        },
        metadata: RawMetadataSnapshot {
            make: "Pentax".into(),
            model: "K10D".into(),
            normalized_make: "Pentax".into(),
            normalized_model: "K10D".into(),
            dng_version: Some("1.1.0.0".into()),
            raw_count: 1,
            raw_dimensions: ImageDimensions {
                width: 3_936,
                height: 2_624,
            },
            image_dimensions: ImageDimensions {
                width: 3_896,
                height: 2_616,
            },
            margins: ImageMargins::default(),
            orientation: 0,
            cfa_pattern: "RGGB".into(),
            sensor_colors: 3,
            sensor_bits: 12,
            black_level: 0,
            white_level: 4_095,
            as_shot_neutral: [0.64, 1.0, 0.966, 0.0],
            baseline_exposure: -0.5,
            iso_speed: 100.0,
            exposure_time_seconds: 1.0 / 125.0,
            aperture_f_number: 5.6,
            focal_length_mm: 35.0,
            captured_at_unix_seconds: 1_700_000_000,
            lens_make: "Pentax".into(),
            lens_model: "smc PENTAX-DA 35mm".into(),
            focal_length_35mm: 52.0,
        },
        capabilities: DecodeCapabilitySnapshot {
            metadata: DecodeSupport::Available,
            embedded_previews: DecodeSupport::Available,
            raw_frame: DecodeSupport::Available,
            reference_rgb: DecodeSupport::Available,
            pending_corrections: PendingCorrectionsSnapshot::default(),
            raw_development: RawDevelopmentCapabilitySnapshot::default(),
        },
        previews: preview_ids
            .iter()
            .map(|provider_id| PreviewDescriptorSnapshot {
                provider_id: *provider_id,
                codec: PreviewCodec::Jpeg,
                dimensions: ImageDimensions {
                    width: 3_872,
                    height: 2_592,
                },
                bits_per_channel: 8,
                channels: 3,
                encoded_bytes: 1_285_213,
                decodable: true,
            })
            .collect(),
    }
}
