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
    /// An owned, unprocessed sensor-domain `RawFrame` is available. This is not a claim that
    /// the source uses Bayer, only that RAW-domain stages can inspect its explicit frame layout.
    pub raw_frame: DecodeSupport,
    pub reference_rgb: DecodeSupport,
    pub pending_corrections: PendingCorrectionsSnapshot,
    /// Provider-neutral source-development capabilities. This is deliberately
    /// distinct from `raw_frame`: a provider can expose sensor samples while still
    /// declining a particular development intent or opcode policy.
    #[serde(default)]
    pub raw_development: RawDevelopmentCapabilitySnapshot,
}

/// Stable, decoder-neutral capabilities for negotiating a `RawDevelopmentPlan`.
///
/// The bit masks retain forward compatibility with new plan enum values without
/// making catalog snapshots depend on one concrete decoder library. A zero mask
/// means no support is claimed; it never means "use provider defaults".
#[derive(Debug, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct RawDevelopmentCapabilitySnapshot {
    #[serde(default)]
    pub plan_schema_version: u32,
    #[serde(default)]
    pub available: DecodeSupport,
    #[serde(default)]
    pub raw_frame: DecodeSupport,
    #[serde(default)]
    pub dng_opcode_execution_receipt: DecodeSupport,
    #[serde(default)]
    pub supported_intents: u32,
    #[serde(default)]
    pub supported_qualities: u32,
    #[serde(default)]
    pub supported_dng_opcode_policies: u32,
    #[serde(default)]
    pub supported_noise_reduction_intents: u32,
    #[serde(default)]
    pub supported_highlight_recovery_intents: u32,
}

impl Default for RawDevelopmentCapabilitySnapshot {
    fn default() -> Self {
        Self {
            plan_schema_version: 0,
            available: DecodeSupport::Unavailable,
            raw_frame: DecodeSupport::Unavailable,
            dng_opcode_execution_receipt: DecodeSupport::Unavailable,
            supported_intents: 0,
            supported_qualities: 0,
            supported_dng_opcode_policies: 0,
            supported_noise_reduction_intents: 0,
            supported_highlight_recovery_intents: 0,
        }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DecodeSupport {
    Unavailable,
    Available,
}

impl Default for DecodeSupport {
    fn default() -> Self {
        Self::Unavailable
    }
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
    #[serde(default)]
    pub iso_speed: f64,
    #[serde(default)]
    pub exposure_time_seconds: f64,
    #[serde(default)]
    pub aperture_f_number: f64,
    #[serde(default)]
    pub focal_length_mm: f64,
    #[serde(default)]
    pub captured_at_unix_seconds: i64,
    #[serde(default)]
    pub lens_make: String,
    #[serde(default)]
    pub lens_model: String,
    #[serde(default)]
    pub focal_length_35mm: f64,
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

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ProxyPayload {
    pub dimensions: ImageDimensions,
    pub codec: PreviewCodec,
    pub bits_per_channel: u16,
    pub channels: u16,
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
}
