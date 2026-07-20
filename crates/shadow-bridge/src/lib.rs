//! Safe, coarse-grained Rust access to Shadow's C++ image decoder providers.

use std::path::{Path, PathBuf};

use shadow_domain::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot,
    ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PreviewCodec,
    PreviewDescriptorSnapshot, RawMetadataSnapshot,
};
use thiserror::Error;

#[cxx::bridge(namespace = "shadow::bridge")]
mod ffi {
    #[derive(Debug)]
    enum FfiPreviewFormat {
        Unknown,
        Jpeg,
        Bitmap,
        JpegXl,
        H265,
    }

    #[derive(Debug)]
    struct FfiDimensions {
        width: u32,
        height: u32,
    }

    #[derive(Debug)]
    struct FfiMargins {
        left: u32,
        top: u32,
        right: u32,
        bottom: u32,
    }

    #[derive(Debug)]
    struct FfiProviderSnapshot {
        id: String,
        version: String,
        dng_sdk: bool,
        rawspeed: bool,
        jpeg: bool,
    }

    #[derive(Debug)]
    struct FfiMetadataSnapshot {
        make: String,
        model: String,
        normalized_make: String,
        normalized_model: String,
        dng_version: String,
        raw_count: u32,
        raw_dimensions: FfiDimensions,
        image_dimensions: FfiDimensions,
        margins: FfiMargins,
        orientation: i32,
        cfa_pattern: String,
        sensor_colors: u32,
        sensor_bits: u32,
        black_level: u32,
        white_level: u32,
        as_shot_neutral_r: f64,
        as_shot_neutral_g1: f64,
        as_shot_neutral_b: f64,
        as_shot_neutral_g2: f64,
        baseline_exposure: f64,
    }

    #[derive(Debug)]
    #[allow(clippy::struct_excessive_bools)]
    struct FfiCapabilitySnapshot {
        metadata: bool,
        embedded_previews: bool,
        mosaic: bool,
        reference_rgb: bool,
        dng_opcode_list_1_bytes: u32,
        dng_opcode_list_2_bytes: u32,
        dng_opcode_list_3_bytes: u32,
    }

    #[derive(Debug)]
    struct FfiPreviewSnapshot {
        provider_id: usize,
        format: FfiPreviewFormat,
        dimensions: FfiDimensions,
        bits_per_channel: u16,
        channels: u16,
        encoded_bytes: u64,
        decodable: bool,
    }

    unsafe extern "C++" {
        include!("shadow/image/cxx_bridge.hpp");

        type DecodeHandle;

        fn open_libraw_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn provider(self: &DecodeHandle) -> FfiProviderSnapshot;
        fn metadata(self: &DecodeHandle) -> FfiMetadataSnapshot;
        fn capabilities(self: &DecodeHandle) -> FfiCapabilitySnapshot;
        fn previews(self: &DecodeHandle) -> Vec<FfiPreviewSnapshot>;
    }
}

#[derive(Debug, Error)]
pub enum BridgeError {
    #[error("the Mac-first decoder bridge currently requires a UTF-8 path: {0}")]
    NonUtf8Path(PathBuf),
    #[error("the C++ decoder bridge returned a null handle")]
    NullHandle,
    #[error("C++ decoder error: {0}")]
    Decoder(#[from] cxx::Exception),
}

/// Opens a RAW file through the `LibRaw` provider and copies a stable, owned
/// descriptor snapshot into Rust. Pixel buffers intentionally remain on the
/// C++ side at this stage.
///
/// # Errors
///
/// Returns [`BridgeError::NonUtf8Path`] for paths not yet representable by the
/// Mac-first bridge, or a decoder error when the C++ provider cannot open and
/// identify the file.
pub fn inspect_libraw(path: &Path) -> Result<DecoderSnapshot, BridgeError> {
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    let handle = ffi::open_libraw_utf8(utf8_path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;

    let provider = handle.provider();
    let metadata = handle.metadata();
    let capabilities = handle.capabilities();
    let previews = handle.previews();

    Ok(DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: provider.id,
            version: provider.version,
            dng_sdk: provider.dng_sdk,
            rawspeed: provider.rawspeed,
            jpeg: provider.jpeg,
        },
        metadata: RawMetadataSnapshot {
            make: metadata.make,
            model: metadata.model,
            normalized_make: metadata.normalized_make,
            normalized_model: metadata.normalized_model,
            dng_version: (!metadata.dng_version.is_empty()).then_some(metadata.dng_version),
            raw_count: metadata.raw_count,
            raw_dimensions: dimensions(&metadata.raw_dimensions),
            image_dimensions: dimensions(&metadata.image_dimensions),
            margins: ImageMargins {
                left: metadata.margins.left,
                top: metadata.margins.top,
                right: metadata.margins.right,
                bottom: metadata.margins.bottom,
            },
            orientation: metadata.orientation,
            cfa_pattern: metadata.cfa_pattern,
            sensor_colors: metadata.sensor_colors,
            sensor_bits: metadata.sensor_bits,
            black_level: metadata.black_level,
            white_level: metadata.white_level,
            as_shot_neutral: [
                metadata.as_shot_neutral_r,
                metadata.as_shot_neutral_g1,
                metadata.as_shot_neutral_b,
                metadata.as_shot_neutral_g2,
            ],
            baseline_exposure: metadata.baseline_exposure,
        },
        capabilities: DecodeCapabilitySnapshot {
            metadata: support(capabilities.metadata),
            embedded_previews: support(capabilities.embedded_previews),
            mosaic: support(capabilities.mosaic),
            reference_rgb: support(capabilities.reference_rgb),
            pending_corrections: PendingCorrectionsSnapshot {
                dng_opcode_list_bytes: [
                    capabilities.dng_opcode_list_1_bytes,
                    capabilities.dng_opcode_list_2_bytes,
                    capabilities.dng_opcode_list_3_bytes,
                ],
            },
        },
        previews: previews
            .into_iter()
            .map(|preview| PreviewDescriptorSnapshot {
                provider_id: preview.provider_id,
                codec: preview_codec(preview.format),
                dimensions: dimensions(&preview.dimensions),
                bits_per_channel: preview.bits_per_channel,
                channels: preview.channels,
                encoded_bytes: preview.encoded_bytes,
                decodable: preview.decodable,
            })
            .collect(),
    })
}

fn dimensions(value: &ffi::FfiDimensions) -> ImageDimensions {
    ImageDimensions {
        width: value.width,
        height: value.height,
    }
}

fn preview_codec(value: ffi::FfiPreviewFormat) -> PreviewCodec {
    match value {
        ffi::FfiPreviewFormat::Jpeg => PreviewCodec::Jpeg,
        ffi::FfiPreviewFormat::Bitmap => PreviewCodec::Bitmap,
        ffi::FfiPreviewFormat::JpegXl => PreviewCodec::JpegXl,
        ffi::FfiPreviewFormat::H265 => PreviewCodec::H265,
        _ => PreviewCodec::Unknown,
    }
}

const fn support(value: bool) -> DecodeSupport {
    if value {
        DecodeSupport::Available
    } else {
        DecodeSupport::Unavailable
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
    fn real_dng_snapshot_crosses_the_bridge() {
        let path = std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG");
        let snapshot = inspect_libraw(Path::new(&path)).expect("inspect local DNG");
        assert_eq!(snapshot.provider.id, "libraw");
        assert!(snapshot.capabilities.metadata.is_available());
        assert!(snapshot.capabilities.mosaic.is_available());
        assert!(snapshot.metadata.raw_dimensions.pixel_count() > 0);
    }
}
