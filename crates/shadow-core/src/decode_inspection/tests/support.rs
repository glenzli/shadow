use std::{fs, path::PathBuf};

use shadow_catalog::{RegisterAsset, RepresentationFingerprint};
use shadow_domain::{
    AssetLocation, DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport,
    DecoderSnapshot, EntityId, ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PhotoId,
    Platform, PreviewCodec, PreviewDescriptorSnapshot, RawDevelopmentCapabilitySnapshot,
    RawMetadataSnapshot, RepresentationKind,
};

use crate::display_jpeg_fixture::DISPLAY_JPEG_BYTES;

#[derive(Debug)]
pub(super) struct Fixture {
    pub(super) root: PathBuf,
    pub(super) raw_path: PathBuf,
    pub(super) database_path: PathBuf,
}

impl Fixture {
    pub(super) fn new() -> Self {
        let root = std::env::temp_dir().join(format!("shadow-decode-{}", PhotoId::new_v7()));
        fs::create_dir_all(&root).expect("create fixture directory");
        let raw_path = root.join("input.dng");
        fs::write(&raw_path, b"fixture").expect("write fixture");
        Self {
            database_path: root.join("catalog.sqlite"),
            root,
            raw_path,
        }
    }

    pub(super) fn registration(&self, source: RepresentationFingerprint) -> RegisterAsset {
        RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                self.raw_path.as_os_str().as_encoded_bytes().to_vec(),
                self.raw_path.display().to_string(),
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 100,
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.root).expect("remove fixture directory");
    }
}

pub(super) fn sample_snapshot() -> DecoderSnapshot {
    DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: "anonymous".into(),
            version: "1".into(),
            dng_sdk: false,
            rawspeed: false,
            jpeg: false,
        },
        metadata: RawMetadataSnapshot {
            make: "Test".into(),
            model: "Fixture".into(),
            normalized_make: "Test".into(),
            normalized_model: "Fixture".into(),
            dng_version: Some("1.4.0.0".into()),
            raw_count: 1,
            raw_dimensions: ImageDimensions {
                width: 10,
                height: 10,
            },
            image_dimensions: ImageDimensions {
                width: 10,
                height: 10,
            },
            margins: ImageMargins::default(),
            orientation: 0,
            cfa_pattern: "RGGB".into(),
            sensor_colors: 3,
            sensor_bits: 12,
            black_level: 0,
            white_level: 4_095,
            as_shot_neutral: [1.0; 4],
            baseline_exposure: 0.0,
            iso_speed: 0.0,
            exposure_time_seconds: 0.0,
            aperture_f_number: 0.0,
            focal_length_mm: 0.0,
            focus_observation: None,
            captured_at_unix_seconds: 0,
            gps: None,
            lens_make: String::new(),
            lens_model: String::new(),
            focal_length_35mm: 0.0,
        },
        capabilities: DecodeCapabilitySnapshot {
            metadata: DecodeSupport::Available,
            embedded_previews: DecodeSupport::Unavailable,
            raw_frame: DecodeSupport::Available,
            reference_rgb: DecodeSupport::Unavailable,
            pending_corrections: PendingCorrectionsSnapshot::default(),
            raw_development: RawDevelopmentCapabilitySnapshot::default(),
        },
        previews: Vec::new(),
    }
}

pub(super) fn preview_descriptor() -> PreviewDescriptorSnapshot {
    PreviewDescriptorSnapshot {
        provider_id: 7,
        codec: PreviewCodec::Jpeg,
        dimensions: ImageDimensions {
            width: 1_600,
            height: 1_200,
        },
        bits_per_channel: 8,
        channels: 3,
        encoded_bytes: u64::try_from(DISPLAY_JPEG_BYTES.len()).expect("test JPEG length fits"),
        decodable: true,
    }
}
