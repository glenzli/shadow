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
    enum FfiByteOrder {
        NotApplicable,
        Native,
        LittleEndian,
        BigEndian,
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

    #[derive(Debug)]
    struct FfiPreviewPayload {
        present: bool,
        descriptor: FfiPreviewSnapshot,
        byte_order: FfiByteOrder,
        bytes: Vec<u8>,
    }

    #[derive(Debug)]
    struct FfiEncodedProxy {
        dimensions: FfiDimensions,
        format: FfiPreviewFormat,
        bits_per_channel: u16,
        channels: u16,
        bytes: Vec<u8>,
    }

    #[derive(Debug)]
    struct FfiBasicEditRequest {
        exposure_stops: f64,
        contrast_factor: f64,
        red_channel_gain: f64,
        green_channel_gain: f64,
        blue_channel_gain: f64,
        saturation_factor: f64,
        max_edge: u32,
        jpeg_quality: u8,
    }

    unsafe extern "C++" {
        include!("shadow/image/cxx_bridge.hpp");

        type DecodeHandle;

        fn open_libraw_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn libraw_provider_version() -> String;
        fn provider(self: &DecodeHandle) -> FfiProviderSnapshot;
        fn metadata(self: &DecodeHandle) -> FfiMetadataSnapshot;
        fn capabilities(self: &DecodeHandle) -> FfiCapabilitySnapshot;
        fn previews(self: &DecodeHandle) -> Vec<FfiPreviewSnapshot>;
        fn decode_best_preview(self: Pin<&mut DecodeHandle>) -> Result<FfiPreviewPayload>;
        fn render_reference_proxy(
            self: &DecodeHandle,
            max_edge: u32,
            jpeg_quality: u8,
        ) -> Result<FfiEncodedProxy>;
        fn render_edited_reference_proxy(
            self: &DecodeHandle,
            request: &FfiBasicEditRequest,
        ) -> Result<FfiEncodedProxy>;
    }
}

/// Cache-key version for the fixed-order basic edited-preview recipe.
pub const BASIC_EDIT_PREVIEW_RECIPE_VERSION: u32 = 1;

/// The first small, deterministic subset of Shadow's edit graph.
///
/// Execution order is exposure, contrast, resolved RGB channel gains, then
/// saturation. `channel_gains` are post-demosaic scene-linear `[R, G, B]`
/// multipliers. They are deliberately not advertised as RAW white balance.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BasicEditParameters {
    pub exposure_stops: f64,
    pub contrast_factor: f64,
    pub channel_gains: [f64; 3],
    pub saturation_factor: f64,
}

impl Default for BasicEditParameters {
    fn default() -> Self {
        Self {
            exposure_stops: 0.0,
            contrast_factor: 1.0,
            channel_gains: [1.0; 3],
            saturation_factor: 1.0,
        }
    }
}

/// Parameters for a bounded, standard-JPEG edited preview.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EditedProxyRequest {
    pub edits: BasicEditParameters,
    pub max_edge: u32,
    pub jpeg_quality: u8,
}

impl Default for EditedProxyRequest {
    fn default() -> Self {
        Self {
            edits: BasicEditParameters::default(),
            max_edge: 2_048,
            jpeg_quality: 88,
        }
    }
}

impl EditedProxyRequest {
    fn validate(self) -> Result<(), BridgeError> {
        validate_inclusive(
            self.edits.exposure_stops,
            -16.0,
            16.0,
            "exposure_stops must be finite and in -16..=16",
        )?;
        validate_inclusive(
            self.edits.contrast_factor,
            0.0,
            8.0,
            "contrast_factor must be finite and in 0..=8",
        )?;
        for (index, gain) in self.edits.channel_gains.into_iter().enumerate() {
            if !gain.is_finite() || gain <= 0.0 || gain > 16.0 {
                const MESSAGES: [&str; 3] = [
                    "red channel gain must be finite, greater than 0, and at most 16",
                    "green channel gain must be finite, greater than 0, and at most 16",
                    "blue channel gain must be finite, greater than 0, and at most 16",
                ];
                return Err(BridgeError::InvalidEditRequest(MESSAGES[index]));
            }
        }
        validate_inclusive(
            self.edits.saturation_factor,
            0.0,
            8.0,
            "saturation_factor must be finite and in 0..=8",
        )?;
        if !(1..=16_384).contains(&self.max_edge) {
            return Err(BridgeError::InvalidEditRequest(
                "max_edge must be in 1..=16384",
            ));
        }
        if !(1..=100).contains(&self.jpeg_quality) {
            return Err(BridgeError::InvalidEditRequest(
                "jpeg_quality must be in 1..=100",
            ));
        }
        Ok(())
    }
}

fn validate_inclusive(
    value: f64,
    minimum: f64,
    maximum: f64,
    message: &'static str,
) -> Result<(), BridgeError> {
    if value.is_finite() && (minimum..=maximum).contains(&value) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(message))
    }
}

/// Returns the version string of the linked `LibRaw` provider without opening
/// an image.
pub fn libraw_provider_version() -> String {
    ffi::libraw_provider_version()
}

/// Renders a bounded, display-referred JPEG proxy through the `LibRaw`
/// reference path. This is a fallback for RAW files without an embedded
/// preview, not Shadow's eventual scene-linear renderer.
///
/// # Errors
///
/// Returns [`BridgeError`] when the provider cannot render or encode the RAW.
pub fn render_libraw_reference_proxy(
    path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    let handle = open_libraw(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    let proxy = handle.render_reference_proxy(max_edge, jpeg_quality)?;
    Ok(shadow_domain::ProxyPayload {
        dimensions: dimensions(&proxy.dimensions),
        codec: preview_codec(proxy.format),
        bits_per_channel: proxy.bits_per_channel,
        channels: proxy.channels,
        bytes: proxy.bytes,
    })
}

/// Renders the fixed-order basic edit recipe as a bounded standard JPEG.
///
/// The C++ kernel decodes `LibRaw`'s 16-bit sRGB reference output into
/// scene-linear sRGB, executes the adjustment nodes without intermediate
/// clipping, and applies the sRGB transfer function only when encoding the
/// JPEG. This is the first real preview renderer, not the eventual full RAW
/// color pipeline.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidEditRequest`] before RAW I/O for non-finite,
/// out-of-range, or unbounded parameters. Decoder and encoder failures are
/// returned as [`BridgeError::Decoder`].
pub fn render_libraw_edited_proxy(
    path: &Path,
    request: EditedProxyRequest,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    request.validate()?;
    let handle = open_libraw(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    let edits = request.edits;
    let ffi_request = ffi::FfiBasicEditRequest {
        exposure_stops: edits.exposure_stops,
        contrast_factor: edits.contrast_factor,
        red_channel_gain: edits.channel_gains[0],
        green_channel_gain: edits.channel_gains[1],
        blue_channel_gain: edits.channel_gains[2],
        saturation_factor: edits.saturation_factor,
        max_edge: request.max_edge,
        jpeg_quality: request.jpeg_quality,
    };
    let proxy = handle.render_edited_reference_proxy(&ffi_request)?;
    Ok(shadow_domain::ProxyPayload {
        dimensions: dimensions(&proxy.dimensions),
        codec: preview_codec(proxy.format),
        bits_per_channel: proxy.bits_per_channel,
        channels: proxy.channels,
        bytes: proxy.bytes,
    })
}

#[derive(Debug, Error)]
pub enum BridgeError {
    #[error("invalid edited proxy request: {0}")]
    InvalidEditRequest(&'static str),
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
    let handle = open_libraw(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;

    Ok(snapshot(handle))
}

/// Extracts the largest decodable embedded preview selected by the image
/// kernel. Absence of an embedded preview is a successful `None` result.
///
/// # Errors
///
/// Returns [`BridgeError`] when the path cannot cross the Mac-first bridge or
/// the provider fails while decoding the selected preview.
pub fn extract_best_libraw_preview(
    path: &Path,
) -> Result<Option<shadow_domain::PreviewPayload>, BridgeError> {
    let mut handle = open_libraw(path)?;
    if handle.is_null() {
        return Err(BridgeError::NullHandle);
    }
    let payload = handle.pin_mut().decode_best_preview()?;
    if !payload.present {
        return Ok(None);
    }
    Ok(Some(shadow_domain::PreviewPayload {
        descriptor: preview_descriptor(&payload.descriptor),
        byte_order: preview_byte_order(payload.byte_order),
        bytes: payload.bytes,
    }))
}

fn open_libraw(path: &Path) -> Result<cxx::UniquePtr<ffi::DecodeHandle>, BridgeError> {
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    ffi::open_libraw_utf8(utf8_path).map_err(Into::into)
}

fn snapshot(handle: &ffi::DecodeHandle) -> DecoderSnapshot {
    let provider = handle.provider();
    let metadata = handle.metadata();
    let capabilities = handle.capabilities();
    let previews = handle.previews();

    DecoderSnapshot {
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
        previews: previews.iter().map(preview_descriptor).collect(),
    }
}

fn preview_descriptor(preview: &ffi::FfiPreviewSnapshot) -> PreviewDescriptorSnapshot {
    PreviewDescriptorSnapshot {
        provider_id: preview.provider_id,
        codec: preview_codec(preview.format),
        dimensions: dimensions(&preview.dimensions),
        bits_per_channel: preview.bits_per_channel,
        channels: preview.channels,
        encoded_bytes: preview.encoded_bytes,
        decodable: preview.decodable,
    }
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

fn preview_byte_order(value: ffi::FfiByteOrder) -> shadow_domain::PreviewByteOrder {
    match value {
        ffi::FfiByteOrder::Native => shadow_domain::PreviewByteOrder::Native,
        ffi::FfiByteOrder::LittleEndian => shadow_domain::PreviewByteOrder::LittleEndian,
        ffi::FfiByteOrder::BigEndian => shadow_domain::PreviewByteOrder::BigEndian,
        _ => shadow_domain::PreviewByteOrder::NotApplicable,
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
    fn basic_edit_defaults_are_a_bounded_neutral_recipe() {
        let request = EditedProxyRequest::default();
        assert_eq!(request.edits, BasicEditParameters::default());
        assert_eq!(request.max_edge, 2_048);
        assert_eq!(request.jpeg_quality, 88);
        request.validate().expect("neutral recipe is valid");
    }

    #[test]
    fn edited_proxy_parameters_fail_closed_before_raw_io() {
        let invalid_requests = [
            EditedProxyRequest {
                edits: BasicEditParameters {
                    exposure_stops: f64::NAN,
                    ..BasicEditParameters::default()
                },
                ..EditedProxyRequest::default()
            },
            EditedProxyRequest {
                edits: BasicEditParameters {
                    contrast_factor: -0.01,
                    ..BasicEditParameters::default()
                },
                ..EditedProxyRequest::default()
            },
            EditedProxyRequest {
                edits: BasicEditParameters {
                    channel_gains: [1.0, 0.0, 1.0],
                    ..BasicEditParameters::default()
                },
                ..EditedProxyRequest::default()
            },
            EditedProxyRequest {
                edits: BasicEditParameters {
                    saturation_factor: f64::INFINITY,
                    ..BasicEditParameters::default()
                },
                ..EditedProxyRequest::default()
            },
            EditedProxyRequest {
                max_edge: 0,
                ..EditedProxyRequest::default()
            },
            EditedProxyRequest {
                jpeg_quality: 0,
                ..EditedProxyRequest::default()
            },
        ];

        for request in invalid_requests {
            let error = render_libraw_edited_proxy(
                Path::new("fixture-that-must-not-be-opened.raw"),
                request,
            )
            .expect_err("invalid request must fail");
            assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
        }
    }

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

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG_WITH_PREVIEW to point at a local RAW fixture"]
    fn real_dng_embedded_preview_crosses_the_bridge() {
        let path =
            std::env::var_os("SHADOW_TEST_DNG_WITH_PREVIEW").expect("SHADOW_TEST_DNG_WITH_PREVIEW");
        let preview = extract_best_libraw_preview(Path::new(&path))
            .expect("extract local DNG preview")
            .expect("fixture contains a preview");
        assert_eq!(preview.descriptor.codec, PreviewCodec::Jpeg);
        assert!(preview.descriptor.dimensions.pixel_count() > 0);
        assert_eq!(
            preview.descriptor.encoded_bytes,
            u64::try_from(preview.bytes.len()).expect("preview length fits u64")
        );
    }

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG_NO_PREVIEW to point at a local RAW fixture"]
    fn real_dng_reference_proxy_crosses_the_bridge() {
        let path =
            std::env::var_os("SHADOW_TEST_DNG_NO_PREVIEW").expect("SHADOW_TEST_DNG_NO_PREVIEW");
        let proxy = render_libraw_reference_proxy(Path::new(&path), 2_048, 88)
            .expect("render local DNG proxy");
        assert_eq!(proxy.codec, PreviewCodec::Jpeg);
        assert_eq!(proxy.dimensions.width, 2_048);
        assert_eq!(proxy.dimensions.height, 1_536);
        assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
        assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));
    }

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
    fn real_dng_edited_proxy_crosses_the_bridge() {
        let path = std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG");
        let proxy = render_libraw_edited_proxy(
            Path::new(&path),
            EditedProxyRequest {
                edits: BasicEditParameters {
                    exposure_stops: 0.5,
                    contrast_factor: 1.1,
                    channel_gains: [1.05, 1.0, 0.95],
                    saturation_factor: 1.15,
                },
                max_edge: 1_024,
                jpeg_quality: 86,
            },
        )
        .expect("render edited local DNG proxy");
        assert_eq!(proxy.codec, PreviewCodec::Jpeg);
        assert_eq!(proxy.dimensions.width.max(proxy.dimensions.height), 1_024);
        assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
        assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));
    }
}
