use super::*;

#[test]
fn dng_opcodes_are_never_silently_complete() {
    let corrections = PendingCorrectionsSnapshot {
        dng_opcode_list_bytes: [0, 0, 76],
    };
    assert!(corrections.has_pending());
}

#[test]
fn snapshot_round_trips_without_provider_types() {
    let snapshot = DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: "libraw".into(),
            version: "0.22.2".into(),
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
                width: 3936,
                height: 2624,
            },
            image_dimensions: ImageDimensions {
                width: 3896,
                height: 2616,
            },
            margins: ImageMargins {
                left: 0,
                top: 0,
                right: 40,
                bottom: 8,
            },
            orientation: 0,
            cfa_pattern: "RGGB".into(),
            sensor_colors: 3,
            sensor_bits: 12,
            black_level: 0,
            white_level: 4095,
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
        previews: vec![PreviewDescriptorSnapshot {
            provider_id: 1,
            codec: PreviewCodec::Jpeg,
            dimensions: ImageDimensions {
                width: 3872,
                height: 2592,
            },
            bits_per_channel: 8,
            channels: 3,
            encoded_bytes: 1_285_213,
            decodable: true,
        }],
    };

    let json = serde_json::to_string(&snapshot).expect("serialize snapshot");
    let decoded = serde_json::from_str(&json).expect("deserialize snapshot");
    assert_eq!(snapshot, decoded);
}
