use serde::{Deserialize, Serialize};

#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct ImageDimensions {
    pub width: u32,
    pub height: u32,
}

impl ImageDimensions {
    pub const fn pixel_count(self) -> u64 {
        self.width as u64 * self.height as u64
    }
}

#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct ImageMargins {
    pub left: u32,
    pub top: u32,
    pub right: u32,
    pub bottom: u32,
}

#[derive(Debug, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct DecodeProviderSnapshot {
    pub id: String,
    pub version: String,
    pub dng_sdk: bool,
    pub rawspeed: bool,
    pub jpeg: bool,
}

#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct PendingCorrectionsSnapshot {
    pub dng_opcode_list_bytes: [u32; 3],
}

impl PendingCorrectionsSnapshot {
    pub fn has_pending(self) -> bool {
        self.dng_opcode_list_bytes.iter().any(|size| *size != 0)
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct DecodeCapabilitySnapshot {
    pub metadata: DecodeSupport,
    pub embedded_previews: DecodeSupport,
    pub mosaic: DecodeSupport,
    pub reference_rgb: DecodeSupport,
    pub pending_corrections: PendingCorrectionsSnapshot,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DecodeSupport {
    Unavailable,
    Available,
}

impl DecodeSupport {
    pub const fn is_available(self) -> bool {
        matches!(self, Self::Available)
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RawMetadataSnapshot {
    pub make: String,
    pub model: String,
    pub normalized_make: String,
    pub normalized_model: String,
    pub dng_version: Option<String>,
    pub raw_count: u32,
    pub raw_dimensions: ImageDimensions,
    pub image_dimensions: ImageDimensions,
    pub margins: ImageMargins,
    pub orientation: i32,
    pub cfa_pattern: String,
    pub sensor_colors: u32,
    pub sensor_bits: u32,
    pub black_level: u32,
    pub white_level: u32,
    pub as_shot_neutral: [f64; 4],
    pub baseline_exposure: f64,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PreviewCodec {
    Unknown,
    Jpeg,
    Bitmap,
    JpegXl,
    H265,
}

impl PreviewCodec {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Unknown => "unknown",
            Self::Jpeg => "jpeg",
            Self::Bitmap => "bitmap",
            Self::JpegXl => "jpeg_xl",
            Self::H265 => "h265",
        }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PreviewByteOrder {
    NotApplicable,
    Native,
    LittleEndian,
    BigEndian,
}

impl PreviewByteOrder {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::NotApplicable => "not_applicable",
            Self::Native => "native",
            Self::LittleEndian => "little_endian",
            Self::BigEndian => "big_endian",
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct PreviewDescriptorSnapshot {
    pub provider_id: usize,
    pub codec: PreviewCodec,
    pub dimensions: ImageDimensions,
    pub bits_per_channel: u16,
    pub channels: u16,
    pub encoded_bytes: u64,
    pub decodable: bool,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct PreviewPayload {
    pub descriptor: PreviewDescriptorSnapshot,
    pub byte_order: PreviewByteOrder,
    pub bytes: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DecoderSnapshot {
    pub provider: DecodeProviderSnapshot,
    pub metadata: RawMetadataSnapshot,
    pub capabilities: DecodeCapabilitySnapshot,
    pub previews: Vec<PreviewDescriptorSnapshot>,
}

#[cfg(test)]
mod tests {
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
            },
            capabilities: DecodeCapabilitySnapshot {
                metadata: DecodeSupport::Available,
                embedded_previews: DecodeSupport::Available,
                mosaic: DecodeSupport::Available,
                reference_rgb: DecodeSupport::Available,
                pending_corrections: PendingCorrectionsSnapshot::default(),
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
}
