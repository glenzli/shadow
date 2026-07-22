//! Safe, coarse-grained Rust access to Shadow's C++ image decoder providers.

use std::{
    collections::HashSet,
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};
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

    #[derive(Debug, Clone)]
    struct FfiOpticsSettings {
        schema_version: u32,
        enabled: bool,
        correct_distortion: bool,
        correct_tca: bool,
        correct_vignetting: bool,
        automatic_scale: bool,
        camera_profile_maker: String,
        camera_profile_model: String,
        lens_profile_maker: String,
        lens_profile_model: String,
    }

    #[derive(Debug)]
    struct FfiOpticsReceipt {
        status: String,
        provider_id: String,
        provider_version: String,
        camera_profile: String,
        lens_profile: String,
        distortion_available: bool,
        tca_available: bool,
        vignetting_available: bool,
        applied_distortion: bool,
        applied_tca: bool,
        applied_vignetting: bool,
        vignetting_used_distance_fallback: bool,
        applied_scaling: bool,
    }

    // Deliberately a fixed field set: this is source-render provenance rather than an
    // extensible recipe payload. New renderer semantics require a new receipt schema instead
    // of silently overloading a map or an untyped byte document.
    #[derive(Debug)]
    #[allow(clippy::struct_excessive_bools)]
    struct FfiRawDevelopmentReceipt {
        schema_version: u32,
        provider_id: String,
        provider_version: String,
        library_version: String,
        development_settings_signature: String,
        processed_linear_reference_contract_version: u32,
        declared_image_dimensions: FfiDimensions,
        rendered_dimensions: FfiDimensions,
        orientation: i32,
        half_size: bool,
        use_camera_white_balance: bool,
        use_camera_matrix: bool,
        use_auto_brightness: bool,
        use_exposure_correction: bool,
        brightness: f32,
        maximum_adjustment_threshold: f32,
        output_bits_per_channel: u16,
        demosaic_quality: i32,
        output_color: i32,
        gamma_inverse_power: f64,
        gamma_linear_toe_slope: f64,
        dng_opcode_list_1_bytes: u32,
        dng_opcode_list_2_bytes: u32,
        dng_opcode_list_3_bytes: u32,
        process_warnings: u32,
    }

    #[derive(Debug)]
    struct FfiOpticsProfileCandidate {
        camera_maker: String,
        camera_model: String,
        lens_maker: String,
        lens_model: String,
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
        iso_speed: f64,
        exposure_time_seconds: f64,
        aperture_f_number: f64,
        focal_length_mm: f64,
        captured_at_unix_seconds: i64,
        lens_make: String,
        lens_model: String,
        focal_length_35mm: f64,
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
    struct FfiEditPreviewAnalysis {
        version: String,
        sample_dimensions: FfiDimensions,
        red: Vec<u64>,
        green: Vec<u64>,
        blue: Vec<u64>,
        luma: Vec<u64>,
        below_zero_samples: Vec<u64>,
        above_one_samples: Vec<u64>,
        pixel_count: u64,
        shadow_clipped_pixels: u64,
        highlight_clipped_pixels: u64,
    }

    #[derive(Debug)]
    struct FfiAnalyzedEditPreview {
        proxy: FfiEncodedProxy,
        analysis: FfiEditPreviewAnalysis,
    }

    #[derive(Debug)]
    struct FfiDisplayLuma {
        width: u32,
        height: u32,
        stride: u32,
        samples: Vec<f32>,
        preprocessing_version: String,
    }

    #[derive(Debug)]
    enum FfiAdjustmentOperation {
        Exposure,
        Contrast,
        ToneCurve,
        SmoothRgbToneCurve,
        RgbWhiteBalance,
        Saturation,
        SelectiveTone,
        PerceptualColor,
        Lut3D,
        Sharpen,
    }

    #[derive(Debug)]
    struct FfiAdjustmentNode {
        node_id: String,
        operation: FfiAdjustmentOperation,
        parameter_schema_version: u32,
        implementation_version: u32,
        enabled: bool,
        parameters: Vec<f64>,
        /// Operation-specific immutable binary document. Only Lut3D accepts
        /// a validated `.cube` document; every other operation requires empty.
        payload: Vec<u8>,
        /// Lengths for grouped variable-size parameters. Smooth RGB Tone Curve
        /// stores master/R/G/B point counts; Perceptual Color stores the number
        /// of additional sampled color ranges.
        parameter_group_lengths: Vec<u32>,
    }

    #[derive(Debug)]
    struct FfiAdjustmentRenderRequest {
        nodes: Vec<FfiAdjustmentNode>,
        max_edge: u32,
        jpeg_quality: u8,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiDetailTileRect {
        x: u32,
        y: u32,
        width: u32,
        height: u32,
    }

    #[derive(Debug)]
    struct FfiAdjustmentDetailTileRequest {
        nodes: Vec<FfiAdjustmentNode>,
        rect: FfiDetailTileRect,
    }

    #[derive(Debug)]
    struct FfiRenderedDetailTile {
        rect: FfiDetailTileRect,
        full_dimensions: FfiDimensions,
        row_stride_bytes: u32,
        bytes: Vec<u8>,
    }

    unsafe extern "C++" {
        include!("shadow/image/cxx_bridge.hpp");

        type DecodeHandle;
        type EditPreviewHandle;
        type FullEditDetailHandle;

        fn open_libraw_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn open_photo_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn query_libraw_optics_profiles_utf8(path: &str) -> Result<Vec<FfiOpticsProfileCandidate>>;
        fn query_photo_optics_profiles_utf8(path: &str) -> Result<Vec<FfiOpticsProfileCandidate>>;
        fn libraw_provider_version() -> String;
        fn photo_provider_version() -> String;
        fn photo_supported_raster_extensions() -> Vec<String>;
        fn render_photo_reference_proxy(
            path: &str,
            max_edge: u32,
            jpeg_quality: u8,
        ) -> Result<FfiEncodedProxy>;
        fn decode_jpeg_display_luma(encoded: &[u8], max_edge: u32) -> Result<FfiDisplayLuma>;
        fn provider(self: &DecodeHandle) -> FfiProviderSnapshot;
        fn metadata(self: &DecodeHandle) -> FfiMetadataSnapshot;
        fn capabilities(self: &DecodeHandle) -> FfiCapabilitySnapshot;
        // The current public Rust API opens a prepared session directly. Keep this lower-level
        // read-only accessor for C++/future bridge callers, where it reports an explicit default
        // before a source render is prepared.
        #[allow(dead_code)]
        fn raw_development_receipt(self: &DecodeHandle) -> FfiRawDevelopmentReceipt;
        fn previews(self: &DecodeHandle) -> Vec<FfiPreviewSnapshot>;
        fn decode_best_preview(self: Pin<&mut DecodeHandle>) -> Result<FfiPreviewPayload>;
        fn configure_optics(
            self: Pin<&mut DecodeHandle>,
            settings: &FfiOpticsSettings,
        ) -> Result<()>;
        fn render_reference_proxy(
            self: &DecodeHandle,
            max_edge: u32,
            jpeg_quality: u8,
        ) -> Result<FfiEncodedProxy>;
        fn render_adjustment_plan(
            self: &DecodeHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiEncodedProxy>;
        fn prepare_edit_preview(
            self: &DecodeHandle,
            max_edge: u32,
        ) -> Result<UniquePtr<EditPreviewHandle>>;
        fn prepare_edit_detail(self: &DecodeHandle) -> Result<UniquePtr<FullEditDetailHandle>>;
        fn dimensions(self: &EditPreviewHandle) -> FfiDimensions;
        fn max_edge(self: &EditPreviewHandle) -> u32;
        fn optics_receipt(self: &EditPreviewHandle) -> FfiOpticsReceipt;
        fn raw_development_receipt(self: &EditPreviewHandle) -> FfiRawDevelopmentReceipt;
        fn render_adjustment_plan(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiEncodedProxy>;
        fn render_adjustment_plan_with_analysis(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiAnalyzedEditPreview>;
        fn dimensions(self: &FullEditDetailHandle) -> FfiDimensions;
        fn retained_bytes(self: &FullEditDetailHandle) -> u64;
        fn optics_receipt(self: &FullEditDetailHandle) -> FfiOpticsReceipt;
        fn raw_development_receipt(self: &FullEditDetailHandle) -> FfiRawDevelopmentReceipt;
        fn render_adjustment_plan_tile(
            self: &FullEditDetailHandle,
            request: &FfiAdjustmentDetailTileRequest,
        ) -> Result<FfiRenderedDetailTile>;
    }
}

// SAFETY: the C++ handle owns a fully prepared, immutable float working proxy. It contains no
// decoder or borrowed state, its destructor is thread-independent, and every render allocates
// its edit buffer and libjpeg state locally. C++ contract tests exercise repeated const renders;
// the public Rust wrapper exposes no mutable access to the handle.
unsafe impl Send for ffi::EditPreviewHandle {}
// SAFETY: see the Send implementation above. Concurrent calls only read the working proxy.
unsafe impl Sync for ffi::EditPreviewHandle {}

// SAFETY: the C++ handle owns a fully prepared, immutable u16 reference image. It contains no
// decoder or borrowed state, and every tile render allocates independent float/RGB8 buffers.
// The public wrapper exposes no mutable access to the handle.
unsafe impl Send for ffi::FullEditDetailHandle {}
// SAFETY: see the Send implementation above. Concurrent calls only read the retained source.
unsafe impl Sync for ffi::FullEditDetailHandle {}

/// Cache-key version for the fixed-order basic edited-preview recipe.
pub const BASIC_EDIT_PREVIEW_RECIPE_VERSION: u32 = 1;

/// Persisted input-transform contract understood by the C++ optics provider.
pub const OPTICS_SETTINGS_SCHEMA_VERSION: u32 = 1;

#[derive(Debug, Clone, Eq, PartialEq, Hash)]
#[allow(clippy::struct_excessive_bools)] // Mirrors independent persisted correction switches.
pub struct OpticsSettings {
    pub enabled: bool,
    pub correct_distortion: bool,
    pub correct_tca: bool,
    pub correct_vignetting: bool,
    pub automatic_scale: bool,
    pub camera_profile_maker: String,
    pub camera_profile_model: String,
    pub lens_profile_maker: String,
    pub lens_profile_model: String,
}

impl Default for OpticsSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            correct_distortion: true,
            correct_tca: true,
            correct_vignetting: true,
            automatic_scale: true,
            camera_profile_maker: String::new(),
            camera_profile_model: String::new(),
            lens_profile_maker: String::new(),
            lens_profile_model: String::new(),
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
#[allow(clippy::struct_excessive_bools)] // Availability and application are distinct receipt facts.
pub struct OpticsReceipt {
    pub status: String,
    pub provider_id: String,
    pub provider_version: String,
    pub camera_profile: String,
    pub lens_profile: String,
    pub distortion_available: bool,
    pub tca_available: bool,
    pub vignetting_available: bool,
    pub applied_distortion: bool,
    pub applied_tca: bool,
    pub applied_vignetting: bool,
    pub vignetting_used_distance_fallback: bool,
    pub applied_scaling: bool,
}

/// The exact provider-side RAW-development request that produced an editable source raster.
///
/// This is immutable source provenance, not a photographer-editable recipe. A zero
/// `schema_version` is an explicit absence: it means that the active source provider did not
/// report RAW-development provenance. Callers must not infer sensor-domain behavior from it.
///
/// The serialized shape intentionally mirrors the fixed C++ receipt field-for-field. If the
/// source renderer changes what a recorded field means, it must increment
/// `schema_version`; consumers can then preserve rather than misinterpret an unknown receipt.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[allow(clippy::struct_excessive_bools)]
pub struct RawDevelopmentReceipt {
    pub schema_version: u32,
    pub provider_id: String,
    pub provider_version: String,
    pub library_version: String,
    pub development_settings_signature: String,
    pub processed_linear_reference_contract_version: u32,
    pub declared_image_dimensions: ImageDimensions,
    pub rendered_dimensions: ImageDimensions,
    pub orientation: i32,
    pub half_size: bool,
    pub use_camera_white_balance: bool,
    pub use_camera_matrix: bool,
    pub use_auto_brightness: bool,
    pub use_exposure_correction: bool,
    pub brightness: f32,
    pub maximum_adjustment_threshold: f32,
    pub output_bits_per_channel: u16,
    pub demosaic_quality: i32,
    pub output_color: i32,
    pub gamma_inverse_power: f64,
    pub gamma_linear_toe_slope: f64,
    pub declared_dng_opcode_list_bytes: [u32; 3],
    pub process_warnings: u32,
}

impl RawDevelopmentReceipt {
    /// Matches the currently supported C++ `RawDevelopmentReceipt` schema.
    pub const CURRENT_SCHEMA_VERSION: u32 = 1;

    #[must_use]
    pub const fn recorded(&self) -> bool {
        self.schema_version != 0
    }

    #[must_use]
    pub const fn uses_current_schema(&self) -> bool {
        self.schema_version == Self::CURRENT_SCHEMA_VERSION
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct OpticsProfileCandidate {
    pub camera_maker: String,
    pub camera_model: String,
    pub lens_maker: String,
    pub lens_model: String,
}

/// Enumerates Lensfun lenses compatible with the camera identified by a RAW.
/// The result is sorted and deduplicated by stable maker/model identity.
///
/// # Errors
///
/// Returns a path or decoder error when the RAW cannot be opened and inspected.
pub fn query_libraw_optics_profiles(
    path: &Path,
) -> Result<Vec<OpticsProfileCandidate>, BridgeError> {
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    Ok(ffi::query_libraw_optics_profiles_utf8(utf8_path)?
        .into_iter()
        .map(|candidate| OpticsProfileCandidate {
            camera_maker: candidate.camera_maker,
            camera_model: candidate.camera_model,
            lens_maker: candidate.lens_maker,
            lens_model: candidate.lens_model,
        })
        .collect())
}

/// Enumerates optical profiles for a supported photo source selected by Shadow's decoder router.
///
/// RAW sources may return Lensfun candidates. Raster sources return an empty list unless a future
/// source provider can establish safe camera/lens metadata without risking a second correction of
/// already-developed pixels.
///
/// # Errors
///
/// Returns a path or source-router error when the photo cannot be opened and inspected.
pub fn query_photo_optics_profiles(
    path: &Path,
) -> Result<Vec<OpticsProfileCandidate>, BridgeError> {
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    Ok(ffi::query_photo_optics_profiles_utf8(utf8_path)?
        .into_iter()
        .map(|candidate| OpticsProfileCandidate {
            camera_maker: candidate.camera_maker,
            camera_model: candidate.camera_model,
            lens_maker: candidate.lens_maker,
            lens_model: candidate.lens_model,
        })
        .collect())
}

fn ffi_optics_settings(settings: &OpticsSettings) -> ffi::FfiOpticsSettings {
    ffi::FfiOpticsSettings {
        schema_version: OPTICS_SETTINGS_SCHEMA_VERSION,
        enabled: settings.enabled,
        correct_distortion: settings.correct_distortion,
        correct_tca: settings.correct_tca,
        correct_vignetting: settings.correct_vignetting,
        automatic_scale: settings.automatic_scale,
        camera_profile_maker: settings.camera_profile_maker.clone(),
        camera_profile_model: settings.camera_profile_model.clone(),
        lens_profile_maker: settings.lens_profile_maker.clone(),
        lens_profile_model: settings.lens_profile_model.clone(),
    }
}

fn optics_receipt(receipt: ffi::FfiOpticsReceipt) -> OpticsReceipt {
    OpticsReceipt {
        status: receipt.status,
        provider_id: receipt.provider_id,
        provider_version: receipt.provider_version,
        camera_profile: receipt.camera_profile,
        lens_profile: receipt.lens_profile,
        distortion_available: receipt.distortion_available,
        tca_available: receipt.tca_available,
        vignetting_available: receipt.vignetting_available,
        applied_distortion: receipt.applied_distortion,
        applied_tca: receipt.applied_tca,
        applied_vignetting: receipt.applied_vignetting,
        vignetting_used_distance_fallback: receipt.vignetting_used_distance_fallback,
        applied_scaling: receipt.applied_scaling,
    }
}

fn raw_development_receipt(receipt: ffi::FfiRawDevelopmentReceipt) -> RawDevelopmentReceipt {
    RawDevelopmentReceipt {
        schema_version: receipt.schema_version,
        provider_id: receipt.provider_id,
        provider_version: receipt.provider_version,
        library_version: receipt.library_version,
        development_settings_signature: receipt.development_settings_signature,
        processed_linear_reference_contract_version: receipt
            .processed_linear_reference_contract_version,
        declared_image_dimensions: dimensions(&receipt.declared_image_dimensions),
        rendered_dimensions: dimensions(&receipt.rendered_dimensions),
        orientation: receipt.orientation,
        half_size: receipt.half_size,
        use_camera_white_balance: receipt.use_camera_white_balance,
        use_camera_matrix: receipt.use_camera_matrix,
        use_auto_brightness: receipt.use_auto_brightness,
        use_exposure_correction: receipt.use_exposure_correction,
        brightness: receipt.brightness,
        maximum_adjustment_threshold: receipt.maximum_adjustment_threshold,
        output_bits_per_channel: receipt.output_bits_per_channel,
        demosaic_quality: receipt.demosaic_quality,
        output_color: receipt.output_color,
        gamma_inverse_power: receipt.gamma_inverse_power,
        gamma_linear_toe_slope: receipt.gamma_linear_toe_slope,
        declared_dng_opcode_list_bytes: [
            receipt.dng_opcode_list_1_bytes,
            receipt.dng_opcode_list_2_bytes,
            receipt.dng_opcode_list_3_bytes,
        ],
        process_warnings: receipt.process_warnings,
    }
}

/// Hard memory bound for the reusable float working proxy.
///
/// A square proxy at this edge consumes at most 192 MiB for interleaved RGB
/// float32. The intended UI values are 1600 and 2048.
pub const MAX_WARM_EDIT_PREVIEW_EDGE: u32 = 4_096;

/// Number of bins in every warm edit-preview display histogram.
pub const EDIT_PREVIEW_HISTOGRAM_BIN_COUNT: usize = 256;

/// Exact semantic contract for warm edit-preview analysis.
///
/// Histograms cover the complete uncompressed display-encoded sRGB RGB8 warm proxy
/// immediately before JPEG encoding. Clipping counts inspect the edited
/// processed-linear working-RGB samples before display clamping and use strict `< 0` and `> 1`
/// comparisons; exact zero and one are not clipped.
pub const EDIT_PREVIEW_ANALYSIS_VERSION: &str = concat!(
    "shadow.edit-preview-analysis.v1:rgb8-before-jpeg:rec709-encoded-q16:",
    "pre-clamp-linear-strict-lt-gt-any-channel"
);

/// Hard width and height bound for one full-resolution detail tile.
pub const MAX_EDIT_DETAIL_TILE_SIDE: u32 = 1_024;

/// Hard bound for the complete immutable u16 source retained by one detail session.
pub const MAX_EDIT_DETAIL_RETAINED_BYTES: u64 = 512 * 1_024 * 1_024;

/// Hard longest-edge bound for a JPEG display-luma analysis plane.
pub const MAX_JPEG_DISPLAY_LUMA_EDGE: u32 = 512;

/// Stable semantic prefix returned by the JPEG display-luma preprocessor.
///
/// The complete returned version appends `:max-edge-N`, because sharpness
/// observations from different analysis scales are not directly comparable.
pub const JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX: &str = concat!(
    "shadow.jpeg-luma.v2:libjpeg-turbo-",
    env!("SHADOW_LIBJPEG_TURBO_VERSION"),
    ":rgb8:islow:no-fancy-upsampling:no-block-smoothing:assume-srgb:ignore-icc:",
    "stored-orientation:idct-scale-1-2-4-8:bilinear-center-q16:rec709-encoded-q16"
);

/// Owned normalized display-referred luminance decoded from a JPEG proxy.
///
/// This is not RAW sensor luminance. The exact JPEG/color/resize assumptions
/// are carried in [`DecodedDisplayLuma::preprocessing_version`]. `stride` is
/// measured in `f32` samples and is currently always equal to `width`.
#[derive(Debug, Clone, PartialEq)]
pub struct DecodedDisplayLuma {
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub samples: Vec<f32>,
    pub preprocessing_version: String,
}

/// Decodes JPEG bytes into a bounded, tightly packed normalized display-luma
/// plane suitable for `shadow_ai::DisplayLumaPlane`.
///
/// The pipeline uses version-pinned libjpeg-turbo RGB8 output with the integer
/// slow DCT, fancy upsampling and block smoothing disabled. It does not apply
/// ICC profiles or EXIF orientation, assumes encoded sRGB, applies fixed-point
/// Rec.709 luma, selects one of libjpeg's 1/2/4/8 IDCT scales, and
/// deterministically resizes to `max_edge`. Truncated JPEG warnings are
/// rejected rather than repaired.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidDisplayLumaRequest`] unless `max_edge` is in
/// `1..=512`, or [`BridgeError::Decoder`] for corrupt, unsupported, or
/// resource-limited JPEG data.
pub fn decode_jpeg_display_luma(
    encoded: &[u8],
    max_edge: u32,
) -> Result<DecodedDisplayLuma, BridgeError> {
    if !(1..=MAX_JPEG_DISPLAY_LUMA_EDGE).contains(&max_edge) {
        return Err(BridgeError::InvalidDisplayLumaRequest(
            "max_edge must be in 1..=512",
        ));
    }

    let decoded = ffi::decode_jpeg_display_luma(encoded, max_edge)?;
    if decoded.width == 0
        || decoded.height == 0
        || decoded.width > max_edge
        || decoded.height > max_edge
        || decoded.stride != decoded.width
    {
        return Err(BridgeError::InvalidDisplayLumaOutput(
            "dimensions or stride violate the bounded plane contract",
        ));
    }
    let expected_len = usize::try_from(decoded.stride)
        .ok()
        .and_then(|stride| {
            usize::try_from(decoded.height)
                .ok()
                .and_then(|height| stride.checked_mul(height))
        })
        .ok_or(BridgeError::InvalidDisplayLumaOutput(
            "plane length overflows addressable memory",
        ))?;
    if decoded.samples.len() != expected_len {
        return Err(BridgeError::InvalidDisplayLumaOutput(
            "sample length does not match stride times height",
        ));
    }
    if decoded
        .samples
        .iter()
        .any(|sample| !sample.is_finite() || !(0.0..=1.0).contains(sample))
    {
        return Err(BridgeError::InvalidDisplayLumaOutput(
            "samples must be finite and normalized",
        ));
    }
    let expected_version =
        format!("{JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX}:max-edge-{max_edge}");
    if decoded.preprocessing_version != expected_version {
        return Err(BridgeError::InvalidDisplayLumaOutput(
            "preprocessing version does not match the requested scale",
        ));
    }

    Ok(DecodedDisplayLuma {
        width: decoded.width,
        height: decoded.height,
        stride: decoded.stride,
        samples: decoded.samples,
        preprocessing_version: decoded.preprocessing_version,
    })
}

/// Numeric v1 contract used by the existing adjustment operations and the
/// historical linear Tone Curve.
pub const ADJUSTMENT_PARAMETER_SCHEMA_VERSION: u32 = 1;
/// Numeric v1 executor revision. Per-operation v2 contracts must not upgrade
/// unrelated persisted nodes.
pub const ADJUSTMENT_IMPLEMENTATION_VERSION: u32 = 1;
/// Numeric parameter contract for the complete guided scene-linear Selective Tone filter.
/// Its public slider shape remains four normalized values, but its second coefficient-averaging
/// pass must never silently reinterpret the historical pixel-local v1 or one-pass v2 contract.
pub const SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION: u32 = 3;
/// Numeric executor revision for the complete guided Selective Tone filter.
pub const SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION: u32 = 3;
/// Numeric parameter contract for the smooth master-plus-RGB Tone Curve.
pub const SMOOTH_RGB_TONE_CURVE_PARAMETER_SCHEMA_VERSION: u32 = 2;
/// Numeric executor revision for the smooth master-plus-RGB Tone Curve.
pub const SMOOTH_RGB_TONE_CURVE_IMPLEMENTATION_VERSION: u32 = 2;
/// Hard bound for one linearized render plan crossing the language boundary.
pub const MAX_ADJUSTMENT_RENDER_NODES: usize = 256;
/// Hard bound for one immutable `.cube` document crossing the render bridge.
pub const MAX_LUT_DOCUMENT_BYTES: usize = 16 * 1_024 * 1_024;
/// Hard bound for diagnostic node identities crossing the language boundary.
pub const MAX_ADJUSTMENT_NODE_ID_BYTES: usize = 256;
/// Mirrors the CPU reference Tone Curve bound without exposing a C++ type.
pub const MAX_TONE_CURVE_POINTS: usize = 256;
/// Fixed hue anchors used by the first perceptual Color Mixer contract.
pub const COLOR_MIXER_BAND_COUNT: usize = 8;
pub const MAX_POINT_COLOR_RANGES: usize = 16;
pub const PERCEPTUAL_COLOR_V2_PARAMETER_SCHEMA_VERSION: u32 = 2;
pub const PERCEPTUAL_COLOR_V2_IMPLEMENTATION_VERSION: u32 = 2;
/// The visible Detail & Effects payload is still one 33-value FFI record,
/// but Recipe schema 3 compiles it into three internal passes. Their distinct
/// numeric revisions make a C++ executor reject an accidental reordering.
pub const TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION: u32 = 3;
pub const TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION: u32 = 3;
pub const COLOR_GRADING_V3_PARAMETER_SCHEMA_VERSION: u32 = 3;
pub const COLOR_GRADING_V3_IMPLEMENTATION_VERSION: u32 = 4;
pub const FINISHING_EFFECTS_V3_PARAMETER_SCHEMA_VERSION: u32 = 3;
pub const FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION: u32 = 5;
/// Retained only to decode/reject old fixtures explicitly; the current
/// compiler never emits this monolithic contract.
pub const DETAIL_EFFECTS_V2_PARAMETER_SCHEMA_VERSION: u32 = 2;
pub const DETAIL_EFFECTS_V2_IMPLEMENTATION_VERSION: u32 = 2;

/// One authored point shared by the legacy linear and smooth RGB Tone Curve
/// contracts. The surrounding operation version determines interpolation.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ToneCurvePoint {
    pub x: f64,
    pub y: f64,
}

/// Version-2 Tone Curve payload. Every channel stores an explicit curve;
/// neutral channel curves are represented by `(0, 0), (1, 1)` rather than an
/// absent value so persistence and FFI have one canonical shape.
#[derive(Debug, Clone, PartialEq)]
pub struct SmoothRgbToneCurve {
    pub master: Vec<ToneCurvePoint>,
    pub red: Vec<ToneCurvePoint>,
    pub green: Vec<ToneCurvePoint>,
    pub blue: Vec<ToneCurvePoint>,
}

impl Default for SmoothRgbToneCurve {
    fn default() -> Self {
        let identity = || {
            vec![
                ToneCurvePoint { x: 0.0, y: 0.0 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ]
        };
        Self {
            master: identity(),
            red: identity(),
            green: identity(),
            blue: identity(),
        }
    }
}

/// Guided local tonal zones in the processed linear-light RGB working space. Values are normalized
/// user intent in `[-1, 1]`; the executor owns the versioned EV weighting, mask, and strength.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SelectiveToneParameters {
    pub highlights: f64,
    pub shadows: f64,
    pub whites: f64,
    pub blacks: f64,
}

impl Default for SelectiveToneParameters {
    fn default() -> Self {
        Self {
            highlights: 0.0,
            shadows: 0.0,
            whites: 0.0,
            blacks: 0.0,
        }
    }
}

/// One soft, circular hue range layered on top of the fixed Color Mixer.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ColorRangeParameters {
    pub enabled: bool,
    pub center_hue_degrees: f64,
    pub width_degrees: f64,
    pub softness: f64,
    pub hue_shift_degrees: f64,
    pub saturation: f64,
    pub lightness: f64,
}

impl Default for ColorRangeParameters {
    fn default() -> Self {
        Self {
            enabled: false,
            center_hue_degrees: 0.0,
            width_degrees: 30.0,
            softness: 0.5,
            hue_shift_degrees: 0.0,
            saturation: 0.0,
            lightness: 0.0,
        }
    }
}

/// Perceptual color controls evaluated together so Oklab conversion happens
/// once per pixel instead of once per UI slider.
#[derive(Debug, Clone, PartialEq)]
pub struct PerceptualColorParameters {
    pub vibrance: f64,
    pub hue_shifts: [f64; COLOR_MIXER_BAND_COUNT],
    pub saturation: [f64; COLOR_MIXER_BAND_COUNT],
    pub lightness: [f64; COLOR_MIXER_BAND_COUNT],
    pub color_range: ColorRangeParameters,
    pub additional_color_ranges: Vec<ColorRangeParameters>,
}

impl Default for PerceptualColorParameters {
    fn default() -> Self {
        Self {
            vibrance: 0.0,
            hue_shifts: [0.0; COLOR_MIXER_BAND_COUNT],
            saturation: [0.0; COLOR_MIXER_BAND_COUNT],
            lightness: [0.0; COLOR_MIXER_BAND_COUNT],
            color_range: ColorRangeParameters::default(),
            additional_color_ranges: Vec::new(),
        }
    }
}

/// Spatial sharpening, detail, and finishing controls. `radius` is the
/// level-0 Gaussian sigma in pixels; the remaining values are normalized user
/// intent.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SharpenParameters {
    pub amount: f64,
    pub radius: f64,
    pub threshold: f64,
    pub masking: f64,
    pub denoise_luminance: f64,
    pub denoise_detail: f64,
    pub denoise_color: f64,
    pub dehaze: f64,
    pub defringe_purple_amount: f64,
    pub defringe_purple_hue_low: f64,
    pub defringe_purple_hue_high: f64,
    pub defringe_green_amount: f64,
    pub defringe_green_hue_low: f64,
    pub defringe_green_hue_high: f64,
    pub shadows_hue: f64,
    pub shadows_saturation: f64,
    pub shadows_luminance: f64,
    pub midtones_hue: f64,
    pub midtones_saturation: f64,
    pub midtones_luminance: f64,
    pub highlights_hue: f64,
    pub highlights_saturation: f64,
    pub highlights_luminance: f64,
    pub grading_blending: f64,
    pub grading_balance: f64,
    pub grain_amount: f64,
    pub grain_size: f64,
    pub grain_roughness: f64,
    pub vignette_amount: f64,
    pub vignette_midpoint: f64,
    pub vignette_roundness: f64,
    pub vignette_feather: f64,
    pub vignette_highlights: f64,
}

impl Default for SharpenParameters {
    fn default() -> Self {
        Self {
            amount: 0.0,
            radius: 1.0,
            threshold: 0.0,
            masking: 0.0,
            denoise_luminance: 0.0,
            denoise_detail: 0.5,
            denoise_color: 0.0,
            dehaze: 0.0,
            defringe_purple_amount: 0.0,
            defringe_purple_hue_low: 270.0,
            defringe_purple_hue_high: 340.0,
            defringe_green_amount: 0.0,
            defringe_green_hue_low: 100.0,
            defringe_green_hue_high: 165.0,
            shadows_hue: 0.0,
            shadows_saturation: 0.0,
            shadows_luminance: 0.0,
            midtones_hue: 0.0,
            midtones_saturation: 0.0,
            midtones_luminance: 0.0,
            highlights_hue: 0.0,
            highlights_saturation: 0.0,
            highlights_luminance: 0.0,
            grading_blending: 0.5,
            grading_balance: 0.0,
            grain_amount: 0.0,
            grain_size: 0.5,
            grain_roughness: 0.5,
            vignette_amount: 0.0,
            vignette_midpoint: 0.5,
            vignette_roundness: 0.0,
            vignette_feather: 0.5,
            vignette_highlights: 0.0,
        }
    }
}

/// Typed pixel operation in execution order.
#[derive(Debug, Clone, PartialEq)]
pub enum AdjustmentRenderOperation {
    Exposure {
        stops: f64,
    },
    Contrast {
        factor: f64,
        pivot: f64,
    },
    ToneCurve {
        points: Vec<ToneCurvePoint>,
    },
    SmoothRgbToneCurve {
        curves: Box<SmoothRgbToneCurve>,
    },
    RgbWhiteBalance {
        temperature: f64,
        tint: f64,
    },
    Saturation {
        factor: f64,
    },
    SelectiveTone {
        parameters: SelectiveToneParameters,
    },
    PerceptualColor {
        parameters: Box<PerceptualColorParameters>,
    },
    Lut3D {
        document: Vec<u8>,
        intensity: f64,
    },
    Sharpen {
        parameters: Box<SharpenParameters>,
    },
}

/// A bounded, versioned node ready for the C++ reference executor.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentRenderNode {
    pub node_id: String,
    pub parameter_schema_version: u32,
    pub implementation_version: u32,
    pub enabled: bool,
    pub operation: AdjustmentRenderOperation,
}

/// A dependency-ordered linear execution plan.
///
/// Graph topology, stages, masks, layer blending, and shared revisions are
/// deliberately compiled before this boundary. This type contains only the
/// operations the current CPU reference backend can execute. Neighborhood
/// footprint scheduling remains inside the C++ image kernel.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentRenderPlan {
    pub nodes: Vec<AdjustmentRenderNode>,
}

impl AdjustmentRenderPlan {
    /// Validates bounded bridge structure and finite parameter storage.
    /// Operation-specific numerical semantics remain authoritatively checked
    /// by the C++ executor before it touches pixels.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for empty or oversized
    /// plans, duplicate/invalid node ids, unsupported versions, malformed Tone
    /// Curves, or non-finite values.
    pub fn validate(&self) -> Result<(), BridgeError> {
        if self.nodes.is_empty() || self.nodes.len() > MAX_ADJUSTMENT_RENDER_NODES {
            return Err(BridgeError::InvalidEditRequest(
                "adjustment render plan must contain 1 through 256 nodes",
            ));
        }
        let mut node_ids = HashSet::with_capacity(self.nodes.len());
        for node in &self.nodes {
            if node.node_id.trim().is_empty() || node.node_id.len() > MAX_ADJUSTMENT_NODE_ID_BYTES {
                return Err(BridgeError::InvalidEditRequest(
                    "adjustment node id must contain 1 through 256 bytes",
                ));
            }
            if !node_ids.insert(node.node_id.as_str()) {
                return Err(BridgeError::InvalidEditRequest(
                    "adjustment render plan contains duplicate node ids",
                ));
            }
            let contract_matches = match &node.operation {
                AdjustmentRenderOperation::SmoothRgbToneCurve { .. } => {
                    (
                        SMOOTH_RGB_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
                        SMOOTH_RGB_TONE_CURVE_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
                AdjustmentRenderOperation::SelectiveTone { .. } => {
                    (
                        SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION,
                        SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
                AdjustmentRenderOperation::PerceptualColor { .. } => {
                    (
                        PERCEPTUAL_COLOR_V2_PARAMETER_SCHEMA_VERSION,
                        PERCEPTUAL_COLOR_V2_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
                AdjustmentRenderOperation::Sharpen { .. } => {
                    node.parameter_schema_version == TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION
                        && matches!(
                            node.implementation_version,
                            TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION
                                | COLOR_GRADING_V3_IMPLEMENTATION_VERSION
                                | FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION
                        )
                }
                _ => {
                    (
                        ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        ADJUSTMENT_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
            };
            if !contract_matches {
                return Err(BridgeError::InvalidEditRequest(
                    "adjustment node uses an unsupported schema or implementation version",
                ));
            }
            validate_render_operation(&node.operation)?;
        }
        Ok(())
    }
}

#[allow(clippy::float_cmp)] // Tone Curve schemas require exact normalized endpoints.
fn validate_render_operation(operation: &AdjustmentRenderOperation) -> Result<(), BridgeError> {
    match operation {
        AdjustmentRenderOperation::Exposure { stops } => {
            validate_finite_render_parameter(*stops)?;
            let gain = stops.exp2();
            if gain.is_finite() && gain > 0.0 {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "exposure stops must produce a finite, positive gain",
                ))
            }
        }
        AdjustmentRenderOperation::Contrast { factor, pivot } => {
            validate_finite_render_parameter(*factor)?;
            validate_finite_render_parameter(*pivot)?;
            if *factor >= 0.0 && *pivot >= 0.0 {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "contrast factor and pivot must be non-negative",
                ))
            }
        }
        AdjustmentRenderOperation::ToneCurve { points } => validate_tone_curve_points(points),
        AdjustmentRenderOperation::SmoothRgbToneCurve { curves } => {
            for points in [&curves.master, &curves.red, &curves.green, &curves.blue] {
                validate_tone_curve_points(points)?;
            }
            Ok(())
        }
        AdjustmentRenderOperation::RgbWhiteBalance { temperature, tint } => {
            for value in [temperature, tint] {
                validate_finite_render_parameter(*value)?;
                if !(-1.0..=1.0).contains(value) {
                    return Err(BridgeError::InvalidEditRequest(
                        "RGB white balance values must be in -1..=1",
                    ));
                }
            }
            Ok(())
        }
        AdjustmentRenderOperation::Saturation { factor } => {
            validate_finite_render_parameter(*factor)?;
            if *factor >= 0.0 {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "saturation factor must be non-negative",
                ))
            }
        }
        AdjustmentRenderOperation::SelectiveTone { parameters } => {
            validate_selective_tone(*parameters)
        }
        AdjustmentRenderOperation::PerceptualColor { parameters } => {
            validate_perceptual_color(parameters)
        }
        AdjustmentRenderOperation::Lut3D {
            document,
            intensity,
        } => {
            validate_finite_render_parameter(*intensity)?;
            if !(0.0..=1.0).contains(intensity) {
                return Err(BridgeError::InvalidEditRequest(
                    "3D LUT intensity must be in 0..=1",
                ));
            }
            if document.is_empty() {
                if *intensity == 0.0 {
                    Ok(())
                } else {
                    Err(BridgeError::InvalidEditRequest(
                        "an active 3D LUT requires a document",
                    ))
                }
            } else if document.len() <= MAX_LUT_DOCUMENT_BYTES {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "3D LUT document exceeds 16 MiB",
                ))
            }
        }
        AdjustmentRenderOperation::Sharpen { parameters } => validate_sharpen(parameters),
    }
}

#[allow(clippy::float_cmp)] // Tone Curve schemas require exact normalized endpoints.
fn validate_tone_curve_points(points: &[ToneCurvePoint]) -> Result<(), BridgeError> {
    if !(2..=MAX_TONE_CURVE_POINTS).contains(&points.len()) {
        return Err(BridgeError::InvalidEditRequest(
            "tone curve must contain 2 through 256 points",
        ));
    }
    if points.first().is_none_or(|point| point.x != 0.0)
        || points.last().is_none_or(|point| point.x != 1.0)
    {
        return Err(BridgeError::InvalidEditRequest(
            "tone curve x coordinates must start at zero and end at one",
        ));
    }
    let mut previous: Option<ToneCurvePoint> = None;
    for point in points {
        validate_finite_render_parameter(point.x)?;
        validate_finite_render_parameter(point.y)?;
        if let Some(previous_point) = previous {
            if point.x <= previous_point.x {
                return Err(BridgeError::InvalidEditRequest(
                    "tone curve x coordinates must be strictly increasing",
                ));
            }
            let slope = (point.y - previous_point.y) / (point.x - previous_point.x);
            if !slope.is_finite() {
                return Err(BridgeError::InvalidEditRequest(
                    "tone curve segment slopes must be finite",
                ));
            }
        }
        previous = Some(*point);
    }
    Ok(())
}

fn validate_finite_render_parameter(value: f64) -> Result<(), BridgeError> {
    if value.is_finite() {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "adjustment render parameters must be finite",
        ))
    }
}

fn validate_selective_tone(parameters: SelectiveToneParameters) -> Result<(), BridgeError> {
    for value in [
        parameters.highlights,
        parameters.shadows,
        parameters.whites,
        parameters.blacks,
    ] {
        validate_finite_render_parameter(value)?;
        if !(-1.0..=1.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "selective tone values must be in -1..=1",
            ));
        }
    }
    Ok(())
}

fn validate_perceptual_color(parameters: &PerceptualColorParameters) -> Result<(), BridgeError> {
    validate_finite_render_parameter(parameters.vibrance)?;
    if !(-1.0..=1.0).contains(&parameters.vibrance) {
        return Err(BridgeError::InvalidEditRequest(
            "vibrance must be in -1..=1",
        ));
    }
    for values in [
        &parameters.hue_shifts,
        &parameters.saturation,
        &parameters.lightness,
    ] {
        for value in values {
            validate_finite_render_parameter(*value)?;
            if !(-1.0..=1.0).contains(value) {
                return Err(BridgeError::InvalidEditRequest(
                    "Color Mixer values must be in -1..=1",
                ));
            }
        }
    }
    if 1 + parameters.additional_color_ranges.len() > MAX_POINT_COLOR_RANGES {
        return Err(BridgeError::InvalidEditRequest(
            "Point Color supports at most 16 ordered ranges",
        ));
    }
    for range in
        std::iter::once(&parameters.color_range).chain(parameters.additional_color_ranges.iter())
    {
        for value in [
            range.center_hue_degrees,
            range.width_degrees,
            range.softness,
            range.hue_shift_degrees,
            range.saturation,
            range.lightness,
        ] {
            validate_finite_render_parameter(value)?;
        }
        if !(0.0..=360.0).contains(&range.center_hue_degrees)
            || !(1.0..=180.0).contains(&range.width_degrees)
            || !(0.0..=1.0).contains(&range.softness)
            || !(-180.0..=180.0).contains(&range.hue_shift_degrees)
            || !(-1.0..=1.0).contains(&range.saturation)
            || !(-1.0..=1.0).contains(&range.lightness)
        {
            return Err(BridgeError::InvalidEditRequest(
                "perceptual color range parameters are outside their contract",
            ));
        }
    }
    Ok(())
}

fn validate_sharpen(parameters: &SharpenParameters) -> Result<(), BridgeError> {
    for value in [
        parameters.amount,
        parameters.radius,
        parameters.threshold,
        parameters.masking,
        parameters.denoise_luminance,
        parameters.denoise_detail,
        parameters.denoise_color,
        parameters.defringe_purple_amount,
        parameters.defringe_green_amount,
        parameters.shadows_saturation,
        parameters.midtones_saturation,
        parameters.highlights_saturation,
        parameters.grading_blending,
        parameters.grain_amount,
        parameters.grain_size,
        parameters.grain_roughness,
        parameters.vignette_midpoint,
        parameters.vignette_feather,
        parameters.vignette_highlights,
    ] {
        validate_finite_render_parameter(value)?;
    }
    for value in [
        parameters.dehaze,
        parameters.shadows_luminance,
        parameters.midtones_luminance,
        parameters.highlights_luminance,
        parameters.grading_balance,
        parameters.vignette_amount,
        parameters.vignette_roundness,
    ] {
        validate_finite_render_parameter(value)?;
        if !(-1.0..=1.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "signed Detail & Effects values must be in -1..=1",
            ));
        }
    }
    for value in [
        parameters.shadows_hue,
        parameters.midtones_hue,
        parameters.highlights_hue,
    ] {
        validate_finite_render_parameter(value)?;
        if !(0.0..=360.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "Color Grading hue values must be in 0..=360",
            ));
        }
    }
    for value in [
        parameters.defringe_purple_hue_low,
        parameters.defringe_purple_hue_high,
        parameters.defringe_green_hue_low,
        parameters.defringe_green_hue_high,
    ] {
        validate_finite_render_parameter(value)?;
        if !(0.0..=360.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "defringe hue values must be in 0..=360",
            ));
        }
    }
    if parameters.defringe_purple_hue_low + 10.0 > parameters.defringe_purple_hue_high
        || parameters.defringe_green_hue_low + 10.0 > parameters.defringe_green_hue_high
    {
        return Err(BridgeError::InvalidEditRequest(
            "defringe hue ranges must have at least a 10 degree span",
        ));
    }
    if !(0.0..=2.0).contains(&parameters.amount)
        || !(0.1..=5.0).contains(&parameters.radius)
        || !(0.0..=1.0).contains(&parameters.threshold)
        || !(0.0..=1.0).contains(&parameters.masking)
        || [
            parameters.denoise_luminance,
            parameters.denoise_detail,
            parameters.denoise_color,
            parameters.defringe_purple_amount,
            parameters.defringe_green_amount,
            parameters.shadows_saturation,
            parameters.midtones_saturation,
            parameters.highlights_saturation,
            parameters.grading_blending,
            parameters.grain_amount,
            parameters.grain_size,
            parameters.grain_roughness,
            parameters.vignette_midpoint,
            parameters.vignette_feather,
            parameters.vignette_highlights,
        ]
        .into_iter()
        .any(|value| !(0.0..=1.0).contains(&value))
    {
        return Err(BridgeError::InvalidEditRequest(
            "sharpen parameters are outside their contract",
        ));
    }
    Ok(())
}

/// The first small, deterministic subset of Shadow's edit graph.
///
/// Execution order is exposure, contrast, processed-RGB white balance, then
/// saturation. Temperature/tint are creative D65 chromatic adaptation and are
/// deliberately not advertised as sensor-domain RAW white balance.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BasicEditParameters {
    pub exposure_stops: f64,
    pub contrast_factor: f64,
    pub white_balance_temperature: f64,
    pub white_balance_tint: f64,
    pub saturation_factor: f64,
}

impl Default for BasicEditParameters {
    fn default() -> Self {
        Self {
            exposure_stops: 0.0,
            contrast_factor: 1.0,
            white_balance_temperature: 0.0,
            white_balance_tint: 0.0,
            saturation_factor: 1.0,
        }
    }
}

impl BasicEditParameters {
    fn validate(self) -> Result<(), BridgeError> {
        validate_inclusive(
            self.exposure_stops,
            -16.0,
            16.0,
            "exposure_stops must be finite and in -16..=16",
        )?;
        validate_inclusive(
            self.contrast_factor,
            0.0,
            8.0,
            "contrast_factor must be finite and in 0..=8",
        )?;
        validate_inclusive(
            self.white_balance_temperature,
            -1.0,
            1.0,
            "white_balance_temperature must be finite and in -1..=1",
        )?;
        validate_inclusive(
            self.white_balance_tint,
            -1.0,
            1.0,
            "white_balance_tint must be finite and in -1..=1",
        )?;
        validate_inclusive(
            self.saturation_factor,
            0.0,
            8.0,
            "saturation_factor must be finite and in 0..=8",
        )
    }
}

/// Builds the compatibility four-node plan used by the current Precision
/// sliders. New renderer integrations should compile their persisted Recipe
/// directly instead of treating this fixed subset as the source of truth.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidEditRequest`] when a basic parameter violates
/// its public range.
pub fn basic_adjustment_render_plan(
    edits: BasicEditParameters,
) -> Result<AdjustmentRenderPlan, BridgeError> {
    edits.validate()?;
    let node = |node_id: &str, operation| AdjustmentRenderNode {
        node_id: node_id.to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation,
    };
    let plan = AdjustmentRenderPlan {
        nodes: vec![
            node(
                "basic-exposure",
                AdjustmentRenderOperation::Exposure {
                    stops: edits.exposure_stops,
                },
            ),
            node(
                "basic-contrast",
                AdjustmentRenderOperation::Contrast {
                    factor: edits.contrast_factor,
                    pivot: 0.18,
                },
            ),
            node(
                "basic-rgb-white-balance",
                AdjustmentRenderOperation::RgbWhiteBalance {
                    temperature: edits.white_balance_temperature,
                    tint: edits.white_balance_tint,
                },
            ),
            node(
                "basic-saturation",
                AdjustmentRenderOperation::Saturation {
                    factor: edits.saturation_factor,
                },
            ),
        ],
    };
    plan.validate()?;
    Ok(plan)
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
            jpeg_quality: 95,
        }
    }
}

impl EditedProxyRequest {
    fn validate(self) -> Result<(), BridgeError> {
        self.edits.validate()?;
        validate_proxy_max_edge(self.max_edge)?;
        validate_jpeg_quality(self.jpeg_quality)
    }
}

fn validate_warm_edit_max_edge(max_edge: u32) -> Result<(), BridgeError> {
    if (1..=MAX_WARM_EDIT_PREVIEW_EDGE).contains(&max_edge) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "warm edit preview max_edge must be in 1..=4096",
        ))
    }
}

fn validate_jpeg_quality(jpeg_quality: u8) -> Result<(), BridgeError> {
    if (1..=100).contains(&jpeg_quality) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "jpeg_quality must be in 1..=100",
        ))
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

/// Returns the complete generated-image identity without opening an image:
/// linked `LibRaw` version plus Shadow's processed-linear reference-RGB and
/// display-output transform versions.
pub fn libraw_provider_version() -> String {
    ffi::libraw_provider_version()
}

/// Returns the cache-facing identity of Shadow's source-neutral photo router.
///
/// Unlike [`libraw_provider_version`], this identity stays stable across every source type the
/// router accepts. Catalog inspection records and generated-proxy cache entries can therefore
/// share it without pretending every source used the same decoder.
#[must_use]
pub fn photo_provider_version() -> String {
    ffi::photo_provider_version()
}

/// Returns ordinary rendered-image suffixes that the linked native photo router can genuinely
/// open. JPEG is mandatory; HEIF/HEIC is added only when this particular build linked libheif.
#[must_use]
pub fn photo_supported_raster_extensions() -> Vec<String> {
    ffi::photo_supported_raster_extensions()
}

/// A reusable, bounded processed linear-light RGB working proxy for interactive edits.
///
/// [`Self::open`] asks Shadow's source router for processed linear-light sRGB-primary RGB once.
/// The resulting C++ handle retains only an immutable, max-edge-bounded RGB
/// float buffer; it does not retain a decoder or borrow the input path. The
/// handle is both [`Send`] and [`Sync`], and concurrent [`Self::render`] calls
/// use independent edit and JPEG buffers.
pub struct LibRawEditPreviewSession {
    handle: cxx::UniquePtr<ffi::EditPreviewHandle>,
    dimensions: ImageDimensions,
    max_edge: u32,
    raw_development_receipt: RawDevelopmentReceipt,
    optics_receipt: OpticsReceipt,
}

/// Transient analysis of one complete warm-proxy edit render.
///
/// The four histograms are derived from uncompressed display-encoded sRGB RGB8 bytes
/// before JPEG encoding. Per-channel and any-channel clipping counts are
/// derived from the same render's processed-linear working-RGB samples before output clamping;
/// they are not sensor-domain exposure measurements.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditPreviewAnalysis {
    pub version: String,
    pub sample_dimensions: ImageDimensions,
    pub red: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub green: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub blue: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub luma: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub below_zero_samples: [u64; 3],
    pub above_one_samples: [u64; 3],
    pub pixel_count: u64,
    pub shadow_clipped_pixels: u64,
    pub highlight_clipped_pixels: u64,
}

/// A JPEG preview and its generation-matched transient analysis.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct AnalyzedEditPreview {
    pub proxy: shadow_domain::ProxyPayload,
    pub analysis: EditPreviewAnalysis,
}

/// One exact rectangle in the processed full-resolution image coordinate space.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct DetailTileRect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

/// One bounded, unscaled full-resolution tile request.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct DetailTileRequest {
    pub rect: DetailTileRect,
}

impl DetailTileRequest {
    fn validate(self, full_dimensions: ImageDimensions) -> Result<(), BridgeError> {
        let rect = self.rect;
        if rect.width == 0
            || rect.height == 0
            || rect.width > MAX_EDIT_DETAIL_TILE_SIDE
            || rect.height > MAX_EDIT_DETAIL_TILE_SIDE
        {
            return Err(BridgeError::InvalidEditRequest(
                "detail tile width and height must be in 1..=1024",
            ));
        }
        if rect.x >= full_dimensions.width
            || rect.y >= full_dimensions.height
            || rect.width > full_dimensions.width - rect.x
            || rect.height > full_dimensions.height - rect.y
        {
            return Err(BridgeError::InvalidEditRequest(
                "detail tile rectangle must be fully inside the retained image",
            ));
        }
        Ok(())
    }
}

/// Packed display-encoded sRGB RGB8 bytes for one exact full-resolution rectangle.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RenderedDetailTile {
    pub rect: DetailTileRect,
    pub full_dimensions: ImageDimensions,
    pub row_stride_bytes: u32,
    pub bytes: Vec<u8>,
}

/// A reusable immutable full-resolution processed-linear u16 RGB source in sRGB primaries for 1:1 tiles.
///
/// Preparation performs one source-router reference render, retains no decoder, and fails when
/// either the metadata worst-case RGB allocation or the actual retained allocation exceeds
/// 512 MiB. Repeated tile renders convert and edit only the requested rectangle. The wrapper is
/// [`Send`] + [`Sync`], and concurrent renders own independent temporary buffers.
pub struct LibRawEditDetailSession {
    handle: cxx::UniquePtr<ffi::FullEditDetailHandle>,
    dimensions: ImageDimensions,
    retained_bytes: u64,
    raw_development_receipt: RawDevelopmentReceipt,
    optics_receipt: OpticsReceipt,
}

/// Source-neutral name for an immutable interactive photo-editing session.
///
/// The legacy `LibRawEditPreviewSession` name remains available for source compatibility, while
/// the constructor now enters through Shadow's photo router. RAW receipts remain explicit and
/// absent for a raster source that did not perform RAW development.
pub type PhotoEditPreviewSession = LibRawEditPreviewSession;

/// Source-neutral name for an immutable full-resolution photo-detail session.
///
/// See [`PhotoEditPreviewSession`] for the compatibility and RAW-provenance contract.
pub type PhotoEditDetailSession = LibRawEditDetailSession;

impl std::fmt::Debug for LibRawEditDetailSession {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LibRawEditDetailSession")
            .field("dimensions", &self.dimensions)
            .field("retained_bytes", &self.retained_bytes)
            .field("raw_development_receipt", &self.raw_development_receipt)
            .field("optics_receipt", &self.optics_receipt)
            .finish_non_exhaustive()
    }
}

impl std::fmt::Debug for LibRawEditPreviewSession {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LibRawEditPreviewSession")
            .field("dimensions", &self.dimensions)
            .field("max_edge", &self.max_edge)
            .field("raw_development_receipt", &self.raw_development_receipt)
            .field("optics_receipt", &self.optics_receipt)
            .finish_non_exhaustive()
    }
}

impl LibRawEditPreviewSession {
    /// Opens a supported photo into a reusable processed linear-light float RGB proxy in sRGB
    /// primaries.
    ///
    /// `max_edge` must be in `1..=4096`; 1600 or 2048 are the intended UI
    /// values. The bound is checked before the input path is opened.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] before source I/O for an
    /// invalid bound, or a decoder error if preparation fails.
    pub fn open(path: &Path, max_edge: u32) -> Result<Self, BridgeError> {
        Self::open_with_optics(path, max_edge, &OpticsSettings::default())
    }

    /// Opens a reusable preview session with explicit input-stage optical correction settings.
    ///
    /// # Errors
    ///
    /// Returns an invalid-request, path, decoder, resource-limit, or bridge-output error when
    /// validation or preparation fails.
    pub fn open_with_optics(
        path: &Path,
        max_edge: u32,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        validate_warm_edit_max_edge(max_edge)?;
        let mut decode_handle = open_photo(path)?;
        if decode_handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        decode_handle
            .pin_mut()
            .configure_optics(&ffi_optics_settings(optics))?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_preview(max_edge)?;
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let prepared_max_edge = prepared.max_edge();
        let raw_development_receipt = raw_development_receipt(prepared.raw_development_receipt());
        let optics_receipt = optics_receipt(prepared.optics_receipt());

        Ok(Self {
            handle,
            dimensions: prepared_dimensions,
            max_edge: prepared_max_edge,
            raw_development_receipt,
            optics_receipt,
        })
    }

    /// Returns the fixed pixel dimensions of every preview from this session.
    #[must_use]
    pub const fn dimensions(&self) -> ImageDimensions {
        self.dimensions
    }

    /// Returns the requested longest-edge bound used during preparation.
    #[must_use]
    pub const fn max_edge(&self) -> u32 {
        self.max_edge
    }

    /// Returns immutable provenance for the exact RAW development, if any, retained by this
    /// preview.
    #[must_use]
    pub const fn raw_development_receipt(&self) -> &RawDevelopmentReceipt {
        &self.raw_development_receipt
    }

    #[must_use]
    pub const fn optics_receipt(&self) -> &OpticsReceipt {
        &self.optics_receipt
    }

    /// Re-runs only the fixed-order basic nodes and JPEG encoding.
    ///
    /// This method never reopens or decodes the source. Since the prepared working
    /// proxy is immutable, calls may run concurrently from worker threads.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] before entering C++ for
    /// invalid edit values or JPEG quality, and [`BridgeError::Decoder`] for
    /// edit or encoding failures.
    pub fn render(
        &self,
        edits: BasicEditParameters,
        jpeg_quality: u8,
    ) -> Result<shadow_domain::ProxyPayload, BridgeError> {
        let plan = basic_adjustment_render_plan(edits)?;
        self.render_plan(&plan, jpeg_quality)
    }

    /// Executes a dependency-ordered typed plan against the prepared proxy.
    /// This method never reopens or decodes the source.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for an invalid plan or JPEG
    /// quality, and [`BridgeError::Decoder`] for authoritative C++ numeric or
    /// encoding failures.
    pub fn render_plan(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
    ) -> Result<shadow_domain::ProxyPayload, BridgeError> {
        plan.validate()?;
        validate_jpeg_quality(jpeg_quality)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let proxy = handle.render_adjustment_plan(&request)?;
        Ok(proxy_payload(proxy))
    }

    /// Executes a typed plan and returns its JPEG plus generation-matched
    /// display histogram and pre-clamp clipping analysis.
    ///
    /// The analysis covers the complete prepared warm proxy, not the current
    /// viewport. It is computed from uncompressed pixels before JPEG encoding,
    /// so changing `jpeg_quality` cannot change its values.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for an invalid plan or JPEG
    /// quality, [`BridgeError::Decoder`] for authoritative C++ failures, or
    /// [`BridgeError::InvalidEditPreviewOutput`] if any returned analysis field
    /// violates the versioned bridge contract.
    pub fn render_plan_with_analysis(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
    ) -> Result<AnalyzedEditPreview, BridgeError> {
        plan.validate()?;
        validate_jpeg_quality(jpeg_quality)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let analyzed = handle.render_adjustment_plan_with_analysis(&request)?;
        let proxy = proxy_payload(analyzed.proxy);
        validate_analyzed_edit_preview(proxy, analyzed.analysis, self.dimensions)
    }
}

impl LibRawEditDetailSession {
    /// Opens a supported photo into immutable full-resolution processed-linear u16 RGB pixels in
    /// sRGB primaries.
    ///
    /// Provider metadata is checked against the worst-case RGB retention limit before the
    /// reference render starts. The returned allocation is checked independently before it is
    /// retained by the session.
    ///
    /// # Errors
    ///
    /// Returns a path, decoder, resource-limit, or invalid bridge-output error. Sources whose
    /// worst-case or actual retained allocation exceeds 512 MiB fail closed.
    pub fn open(path: &Path) -> Result<Self, BridgeError> {
        Self::open_with_optics(path, &OpticsSettings::default())
    }

    /// Opens a full-resolution detail session with explicit input-stage optical settings.
    ///
    /// # Errors
    ///
    /// Returns a path, decoder, resource-limit, invalid-request, or bridge-output error when
    /// validation or preparation fails.
    pub fn open_with_optics(path: &Path, optics: &OpticsSettings) -> Result<Self, BridgeError> {
        let mut decode_handle = open_photo(path)?;
        if decode_handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        decode_handle
            .pin_mut()
            .configure_optics(&ffi_optics_settings(optics))?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_detail()?;
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let retained_bytes = prepared.retained_bytes();
        let raw_development_receipt = raw_development_receipt(prepared.raw_development_receipt());
        let optics_receipt = optics_receipt(prepared.optics_receipt());
        if prepared_dimensions.width == 0 || prepared_dimensions.height == 0 {
            return Err(BridgeError::InvalidEditDetailOutput(
                "prepared dimensions must be non-zero",
            ));
        }
        if retained_bytes == 0 || retained_bytes > MAX_EDIT_DETAIL_RETAINED_BYTES {
            return Err(BridgeError::InvalidEditDetailOutput(
                "retained bytes must be in 1..=512 MiB",
            ));
        }
        Ok(Self {
            handle,
            dimensions: prepared_dimensions,
            retained_bytes,
            raw_development_receipt,
            optics_receipt,
        })
    }

    /// Returns the processed full-resolution image dimensions used by tile coordinates.
    #[must_use]
    pub const fn dimensions(&self) -> ImageDimensions {
        self.dimensions
    }

    /// Returns the actual immutable u16 allocation retained by this session.
    #[must_use]
    pub const fn retained_bytes(&self) -> u64 {
        self.retained_bytes
    }

    /// Returns immutable provenance for the exact full-resolution RAW development, if any,
    /// retained by this detail session.
    #[must_use]
    pub const fn raw_development_receipt(&self) -> &RawDevelopmentReceipt {
        &self.raw_development_receipt
    }

    #[must_use]
    pub const fn optics_receipt(&self) -> &OpticsReceipt {
        &self.optics_receipt
    }

    /// Executes a dependency-ordered typed plan against one exact full-resolution rectangle.
    ///
    /// Plan and rectangle shape/bounds are rejected in Rust before entering C++. The C++ kernel
    /// validates them again, normalizes only the processed-linear crop to float, executes the
    /// typed nodes, expands neighborhood footprints inside the kernel, and
    /// returns the requested core as tightly packed display-encoded sRGB RGB8
    /// without compression or scaling.
    /// No source I/O occurs during this method.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for an invalid plan or rectangle,
    /// [`BridgeError::Decoder`] for authoritative kernel failures, or
    /// [`BridgeError::InvalidEditDetailOutput`] if bridge output violates its contract.
    pub fn render_plan_tile(
        &self,
        plan: &AdjustmentRenderPlan,
        request: DetailTileRequest,
    ) -> Result<RenderedDetailTile, BridgeError> {
        plan.validate()?;
        request.validate(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let rendered =
            handle.render_adjustment_plan_tile(&ffi_detail_tile_request(plan, request))?;
        let rect = detail_tile_rect(rendered.rect);
        let full_dimensions = dimensions(&rendered.full_dimensions);
        if rect != request.rect || full_dimensions != self.dimensions {
            return Err(BridgeError::InvalidEditDetailOutput(
                "returned identity does not match the requested tile and prepared source",
            ));
        }
        let expected_stride =
            rect.width
                .checked_mul(3)
                .ok_or(BridgeError::InvalidEditDetailOutput(
                    "RGB8 row stride overflows",
                ))?;
        if rendered.row_stride_bytes != expected_stride {
            return Err(BridgeError::InvalidEditDetailOutput(
                "RGB8 row stride must equal width times three",
            ));
        }
        let expected_len = usize::try_from(expected_stride)
            .ok()
            .and_then(|stride| {
                usize::try_from(rect.height)
                    .ok()
                    .and_then(|height| stride.checked_mul(height))
            })
            .ok_or(BridgeError::InvalidEditDetailOutput(
                "RGB8 byte length overflows addressable memory",
            ))?;
        if rendered.bytes.len() != expected_len {
            return Err(BridgeError::InvalidEditDetailOutput(
                "RGB8 byte length must equal row stride times height",
            ));
        }
        Ok(RenderedDetailTile {
            rect,
            full_dimensions,
            row_stride_bytes: rendered.row_stride_bytes,
            bytes: rendered.bytes,
        })
    }
}

/// Renders a bounded, display-referred JPEG proxy through the `LibRaw`
/// reference path. This is a fallback for RAW files without an embedded
/// preview, not Shadow's eventual camera-domain renderer.
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

/// Renders a bounded display-referred JPEG proxy through Shadow's source-neutral photo router.
///
/// The router decides whether the source is developed through a RAW provider or decoded from a
/// supported raster. The returned raster is always the same display-proxy contract; inspect the
/// prepared edit session's [`RawDevelopmentReceipt`] to distinguish provider-side RAW
/// development from a raster source.
///
/// # Errors
///
/// Returns an invalid-request, path, source-router, decode, or encoding error.
pub fn render_photo_reference_proxy(
    path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    validate_proxy_max_edge(max_edge)?;
    validate_jpeg_quality(jpeg_quality)?;
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    Ok(proxy_payload(ffi::render_photo_reference_proxy(
        utf8_path,
        max_edge,
        jpeg_quality,
    )?))
}

/// Renders the fixed-order basic edit recipe as a bounded standard JPEG.
///
/// The C++ kernel configures `LibRaw` to emit 16-bit processed linear-light RGB
/// in sRGB/Rec.709-D65 primaries, normalizes it into the float working proxy,
/// and executes adjustment nodes without intermediate clipping. The versioned
/// display boundary then performs hue-preserving Oklab gamut mapping and the
/// sRGB transfer only while encoding RGB8/JPEG. The `LibRaw` result has already
/// undergone black subtraction, white balance, demosaic, camera-to-output color
/// conversion, and fixed integer scaling (with frame-adaptive maximum disabled);
/// it is not untouched sensor-linear mosaic/radiance data or
/// the eventual full RAW color pipeline.
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
    let plan = basic_adjustment_render_plan(request.edits)?;
    render_libraw_adjustment_plan(path, &plan, request.max_edge, request.jpeg_quality)
}

/// Renders a typed, dependency-ordered adjustment plan as a bounded JPEG.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidEditRequest`] before RAW I/O for an invalid
/// plan, edge bound, or JPEG quality. Decoder, executor, and encoder failures
/// are returned as [`BridgeError::Decoder`].
pub fn render_libraw_adjustment_plan(
    path: &Path,
    plan: &AdjustmentRenderPlan,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    plan.validate()?;
    validate_proxy_max_edge(max_edge)?;
    validate_jpeg_quality(jpeg_quality)?;
    let handle = open_libraw(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    let ffi_request = ffi_render_request(plan, max_edge, jpeg_quality);
    let proxy = handle.render_adjustment_plan(&ffi_request)?;
    Ok(proxy_payload(proxy))
}

fn validate_proxy_max_edge(max_edge: u32) -> Result<(), BridgeError> {
    if (1..=16_384).contains(&max_edge) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "max_edge must be in 1..=16384",
        ))
    }
}

fn ffi_render_request(
    plan: &AdjustmentRenderPlan,
    max_edge: u32,
    jpeg_quality: u8,
) -> ffi::FfiAdjustmentRenderRequest {
    ffi::FfiAdjustmentRenderRequest {
        nodes: plan.nodes.iter().map(ffi_render_node).collect(),
        max_edge,
        jpeg_quality,
    }
}

fn ffi_detail_tile_request(
    plan: &AdjustmentRenderPlan,
    request: DetailTileRequest,
) -> ffi::FfiAdjustmentDetailTileRequest {
    ffi::FfiAdjustmentDetailTileRequest {
        nodes: plan.nodes.iter().map(ffi_render_node).collect(),
        rect: ffi_detail_tile_rect(request.rect),
    }
}

const fn ffi_detail_tile_rect(rect: DetailTileRect) -> ffi::FfiDetailTileRect {
    ffi::FfiDetailTileRect {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
    }
}

const fn detail_tile_rect(rect: ffi::FfiDetailTileRect) -> DetailTileRect {
    DetailTileRect {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
    }
}

// The flat CXX wire record is intentionally assembled in one auditable operation.
#[allow(clippy::too_many_lines)]
fn ffi_render_node(node: &AdjustmentRenderNode) -> ffi::FfiAdjustmentNode {
    let (operation, parameters, parameter_group_lengths, payload) = match &node.operation {
        AdjustmentRenderOperation::Exposure { stops } => (
            ffi::FfiAdjustmentOperation::Exposure,
            vec![*stops],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::Contrast { factor, pivot } => (
            ffi::FfiAdjustmentOperation::Contrast,
            vec![*factor, *pivot],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::ToneCurve { points } => (
            ffi::FfiAdjustmentOperation::ToneCurve,
            points.iter().flat_map(|point| [point.x, point.y]).collect(),
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::SmoothRgbToneCurve { curves } => {
            let channel_point_counts = [&curves.master, &curves.red, &curves.green, &curves.blue]
                .map(|points| {
                    u32::try_from(points.len())
                        .expect("validated Tone Curve channel count always fits u32")
                })
                .to_vec();
            let flattened = [&curves.master, &curves.red, &curves.green, &curves.blue]
                .into_iter()
                .flat_map(|points| points.iter().flat_map(|point| [point.x, point.y]))
                .collect();
            (
                ffi::FfiAdjustmentOperation::SmoothRgbToneCurve,
                flattened,
                channel_point_counts,
                vec![],
            )
        }
        AdjustmentRenderOperation::RgbWhiteBalance { temperature, tint } => (
            ffi::FfiAdjustmentOperation::RgbWhiteBalance,
            vec![*temperature, *tint],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::Saturation { factor } => (
            ffi::FfiAdjustmentOperation::Saturation,
            vec![*factor],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::SelectiveTone { parameters } => (
            ffi::FfiAdjustmentOperation::SelectiveTone,
            vec![
                parameters.highlights,
                parameters.shadows,
                parameters.whites,
                parameters.blacks,
            ],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::PerceptualColor { parameters } => {
            let mut flattened =
                Vec::with_capacity(32 + parameters.additional_color_ranges.len() * 7);
            flattened.push(parameters.vibrance);
            flattened.extend(parameters.hue_shifts);
            flattened.extend(parameters.saturation);
            flattened.extend(parameters.lightness);
            flattened.extend([
                if parameters.color_range.enabled {
                    1.0
                } else {
                    0.0
                },
                parameters.color_range.center_hue_degrees,
                parameters.color_range.width_degrees,
                parameters.color_range.softness,
                parameters.color_range.hue_shift_degrees,
                parameters.color_range.saturation,
                parameters.color_range.lightness,
            ]);
            for range in &parameters.additional_color_ranges {
                flattened.extend([
                    if range.enabled { 1.0 } else { 0.0 },
                    range.center_hue_degrees,
                    range.width_degrees,
                    range.softness,
                    range.hue_shift_degrees,
                    range.saturation,
                    range.lightness,
                ]);
            }
            (
                ffi::FfiAdjustmentOperation::PerceptualColor,
                flattened,
                vec![
                    u32::try_from(parameters.additional_color_ranges.len())
                        .expect("validated Point Color range count fits u32"),
                ],
                vec![],
            )
        }
        AdjustmentRenderOperation::Lut3D {
            document,
            intensity,
        } => (
            ffi::FfiAdjustmentOperation::Lut3D,
            vec![*intensity],
            vec![],
            document.clone(),
        ),
        AdjustmentRenderOperation::Sharpen { parameters } => {
            let mut flattened = vec![
                parameters.amount,
                parameters.radius,
                parameters.threshold,
                parameters.masking,
            ];
            flattened.extend([
                parameters.denoise_luminance,
                parameters.denoise_detail,
                parameters.denoise_color,
                parameters.dehaze,
                parameters.defringe_purple_amount,
                parameters.defringe_purple_hue_low,
                parameters.defringe_purple_hue_high,
                parameters.defringe_green_amount,
                parameters.defringe_green_hue_low,
                parameters.defringe_green_hue_high,
                parameters.shadows_hue,
                parameters.shadows_saturation,
                parameters.shadows_luminance,
                parameters.midtones_hue,
                parameters.midtones_saturation,
                parameters.midtones_luminance,
                parameters.highlights_hue,
                parameters.highlights_saturation,
                parameters.highlights_luminance,
                parameters.grading_blending,
                parameters.grading_balance,
                parameters.grain_amount,
                parameters.grain_size,
                parameters.grain_roughness,
                parameters.vignette_amount,
                parameters.vignette_midpoint,
                parameters.vignette_roundness,
                parameters.vignette_feather,
                parameters.vignette_highlights,
            ]);
            (
                ffi::FfiAdjustmentOperation::Sharpen,
                flattened,
                vec![],
                vec![],
            )
        }
    };
    ffi::FfiAdjustmentNode {
        node_id: node.node_id.clone(),
        operation,
        parameter_schema_version: node.parameter_schema_version,
        implementation_version: node.implementation_version,
        enabled: node.enabled,
        parameters,
        payload,
        parameter_group_lengths,
    }
}

fn proxy_payload(proxy: ffi::FfiEncodedProxy) -> shadow_domain::ProxyPayload {
    shadow_domain::ProxyPayload {
        dimensions: dimensions(&proxy.dimensions),
        codec: preview_codec(proxy.format),
        bits_per_channel: proxy.bits_per_channel,
        channels: proxy.channels,
        bytes: proxy.bytes,
    }
}

fn validate_edit_preview_proxy(
    proxy: &shadow_domain::ProxyPayload,
    expected_dimensions: ImageDimensions,
) -> Result<(), BridgeError> {
    if proxy.dimensions != expected_dimensions
        || proxy.dimensions.width == 0
        || proxy.dimensions.height == 0
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "proxy dimensions must match the prepared warm session",
        ));
    }
    if proxy.codec != PreviewCodec::Jpeg
        || proxy.bits_per_channel != 8
        || proxy.channels != 3
        || !proxy.bytes.starts_with(&[0xff, 0xd8])
        || !proxy.bytes.ends_with(&[0xff, 0xd9])
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "proxy must be a non-empty standard 8-bit three-channel JPEG",
        ));
    }
    Ok(())
}

fn validate_analyzed_edit_preview(
    proxy: shadow_domain::ProxyPayload,
    analysis: ffi::FfiEditPreviewAnalysis,
    expected_dimensions: ImageDimensions,
) -> Result<AnalyzedEditPreview, BridgeError> {
    validate_edit_preview_proxy(&proxy, expected_dimensions)?;
    let analysis = validate_edit_preview_analysis(analysis, expected_dimensions)?;
    Ok(AnalyzedEditPreview { proxy, analysis })
}

fn validated_histogram(
    bins: Vec<u64>,
    pixel_count: u64,
) -> Result<[u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT], BridgeError> {
    let bins: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT] = bins.try_into().map_err(|_| {
        BridgeError::InvalidEditPreviewOutput("every histogram must contain exactly 256 bins")
    })?;
    let sum = bins.iter().try_fold(0_u64, |sum, count| {
        sum.checked_add(*count)
            .ok_or(BridgeError::InvalidEditPreviewOutput(
                "histogram sample count overflows u64",
            ))
    })?;
    if sum != pixel_count {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "every histogram sum must equal pixel_count",
        ));
    }
    Ok(bins)
}

fn validated_channel_counts(values: Vec<u64>) -> Result<[u64; 3], BridgeError> {
    values.try_into().map_err(|_| {
        BridgeError::InvalidEditPreviewOutput(
            "per-channel clipping counts must contain exactly three values",
        )
    })
}

fn validate_any_channel_clip_count(
    per_channel: &[u64; 3],
    any_channel: u64,
    pixel_count: u64,
) -> Result<(), BridgeError> {
    let per_channel_sum = per_channel.iter().try_fold(0_u64, |sum, count| {
        if *count > pixel_count {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "per-channel clipping count exceeds pixel_count",
            ));
        }
        sum.checked_add(*count)
            .ok_or(BridgeError::InvalidEditPreviewOutput(
                "clipping sample count overflows u64",
            ))
    })?;
    let largest_channel = per_channel.iter().copied().max().unwrap_or(0);
    if any_channel > pixel_count || any_channel < largest_channel || any_channel > per_channel_sum {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "any-channel clipping count violates per-channel bounds",
        ));
    }
    Ok(())
}

fn validate_edit_preview_analysis(
    analysis: ffi::FfiEditPreviewAnalysis,
    proxy_dimensions: ImageDimensions,
) -> Result<EditPreviewAnalysis, BridgeError> {
    if analysis.version != EDIT_PREVIEW_ANALYSIS_VERSION {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "analysis version is unsupported",
        ));
    }
    let sample_dimensions = dimensions(&analysis.sample_dimensions);
    if sample_dimensions.width == 0
        || sample_dimensions.height == 0
        || sample_dimensions != proxy_dimensions
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "analysis dimensions must be non-zero and match the proxy",
        ));
    }
    if analysis.pixel_count != sample_dimensions.pixel_count() {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "analysis pixel_count must equal width times height",
        ));
    }

    let red = validated_histogram(analysis.red, analysis.pixel_count)?;
    let green = validated_histogram(analysis.green, analysis.pixel_count)?;
    let blue = validated_histogram(analysis.blue, analysis.pixel_count)?;
    let luma = validated_histogram(analysis.luma, analysis.pixel_count)?;
    let below_zero_samples = validated_channel_counts(analysis.below_zero_samples)?;
    let above_one_samples = validated_channel_counts(analysis.above_one_samples)?;

    for channel in 0..3 {
        if below_zero_samples[channel]
            .checked_add(above_one_samples[channel])
            .is_none_or(|total| total > analysis.pixel_count)
        {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "one channel cannot be below zero and above one for the same pixel",
            ));
        }
    }
    validate_any_channel_clip_count(
        &below_zero_samples,
        analysis.shadow_clipped_pixels,
        analysis.pixel_count,
    )?;
    validate_any_channel_clip_count(
        &above_one_samples,
        analysis.highlight_clipped_pixels,
        analysis.pixel_count,
    )?;

    Ok(EditPreviewAnalysis {
        version: analysis.version,
        sample_dimensions,
        red,
        green,
        blue,
        luma,
        below_zero_samples,
        above_one_samples,
        pixel_count: analysis.pixel_count,
        shadow_clipped_pixels: analysis.shadow_clipped_pixels,
        highlight_clipped_pixels: analysis.highlight_clipped_pixels,
    })
}

#[derive(Debug, Error)]
pub enum BridgeError {
    #[error("invalid edited proxy request: {0}")]
    InvalidEditRequest(&'static str),
    #[error("invalid edit-preview analysis bridge output: {0}")]
    InvalidEditPreviewOutput(&'static str),
    #[error("invalid full edit detail bridge output: {0}")]
    InvalidEditDetailOutput(&'static str),
    #[error("invalid JPEG display-luma request: {0}")]
    InvalidDisplayLumaRequest(&'static str),
    #[error("invalid JPEG display-luma decoder output: {0}")]
    InvalidDisplayLumaOutput(&'static str),
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

/// Inspects a supported photo through Shadow's source-neutral decoder router.
///
/// The returned snapshot identifies the router as the cache-facing provider. The exact RAW
/// renderer, when any, remains separately available as a prepared session's immutable
/// [`RawDevelopmentReceipt`].
///
/// # Errors
///
/// Returns a path or source-router error when the photo cannot be opened and identified.
pub fn inspect_photo(path: &Path) -> Result<DecoderSnapshot, BridgeError> {
    let handle = open_photo(path)?;
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

/// Extracts the router-selected embedded preview from a supported photo source.
///
/// Absence of an embedded preview is a successful `None` result. A raster source may choose to
/// expose no embedded preview and instead rely on [`render_photo_reference_proxy`].
///
/// # Errors
///
/// Returns a path or source-router error when the source cannot be opened or decoded.
pub fn extract_best_photo_preview(
    path: &Path,
) -> Result<Option<shadow_domain::PreviewPayload>, BridgeError> {
    let mut handle = open_photo(path)?;
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

fn open_photo(path: &Path) -> Result<cxx::UniquePtr<ffi::DecodeHandle>, BridgeError> {
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    ffi::open_photo_utf8(utf8_path).map_err(Into::into)
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
            iso_speed: metadata.iso_speed,
            exposure_time_seconds: metadata.exposure_time_seconds,
            aperture_f_number: metadata.aperture_f_number,
            focal_length_mm: metadata.focal_length_mm,
            captured_at_unix_seconds: metadata.captured_at_unix_seconds,
            lens_make: metadata.lens_make,
            lens_model: metadata.lens_model,
            focal_length_35mm: metadata.focal_length_35mm,
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

    fn recorded_ffi_raw_development_receipt() -> ffi::FfiRawDevelopmentReceipt {
        ffi::FfiRawDevelopmentReceipt {
            schema_version: RawDevelopmentReceipt::CURRENT_SCHEMA_VERSION,
            provider_id: "fixture-provider".to_owned(),
            provider_version: "fixture-provider-v1".to_owned(),
            library_version: "fixture-library-v1".to_owned(),
            development_settings_signature: "fixture-request-v1".to_owned(),
            processed_linear_reference_contract_version: 7,
            declared_image_dimensions: ffi::FfiDimensions {
                width: 8,
                height: 4,
            },
            rendered_dimensions: ffi::FfiDimensions {
                width: 4,
                height: 2,
            },
            orientation: 5,
            half_size: true,
            use_camera_white_balance: true,
            use_camera_matrix: true,
            use_auto_brightness: false,
            use_exposure_correction: false,
            brightness: 1.25,
            maximum_adjustment_threshold: 0.125,
            output_bits_per_channel: 16,
            demosaic_quality: 3,
            output_color: 1,
            gamma_inverse_power: 1.0,
            gamma_linear_toe_slope: 1.0,
            dng_opcode_list_1_bytes: 11,
            dng_opcode_list_2_bytes: 22,
            dng_opcode_list_3_bytes: 33,
            process_warnings: 0x1024,
        }
    }

    #[test]
    #[allow(clippy::float_cmp)] // The bridge contract preserves native scalar bits verbatim.
    fn raw_development_receipt_bridge_preserves_default_and_recorded_fields() {
        let default = raw_development_receipt(ffi::FfiRawDevelopmentReceipt {
            schema_version: 0,
            provider_id: String::new(),
            provider_version: String::new(),
            library_version: String::new(),
            development_settings_signature: String::new(),
            processed_linear_reference_contract_version: 0,
            declared_image_dimensions: ffi::FfiDimensions {
                width: 0,
                height: 0,
            },
            rendered_dimensions: ffi::FfiDimensions {
                width: 0,
                height: 0,
            },
            orientation: 0,
            half_size: false,
            use_camera_white_balance: false,
            use_camera_matrix: false,
            use_auto_brightness: false,
            use_exposure_correction: false,
            brightness: 0.0,
            maximum_adjustment_threshold: 0.0,
            output_bits_per_channel: 0,
            demosaic_quality: 0,
            output_color: 0,
            gamma_inverse_power: 0.0,
            gamma_linear_toe_slope: 0.0,
            dng_opcode_list_1_bytes: 0,
            dng_opcode_list_2_bytes: 0,
            dng_opcode_list_3_bytes: 0,
            process_warnings: 0,
        });
        assert_eq!(default, RawDevelopmentReceipt::default());
        assert!(!default.recorded());
        assert!(!default.uses_current_schema());

        let recorded = raw_development_receipt(recorded_ffi_raw_development_receipt());
        assert_eq!(
            recorded,
            RawDevelopmentReceipt {
                schema_version: 1,
                provider_id: "fixture-provider".to_owned(),
                provider_version: "fixture-provider-v1".to_owned(),
                library_version: "fixture-library-v1".to_owned(),
                development_settings_signature: "fixture-request-v1".to_owned(),
                processed_linear_reference_contract_version: 7,
                declared_image_dimensions: ImageDimensions {
                    width: 8,
                    height: 4,
                },
                rendered_dimensions: ImageDimensions {
                    width: 4,
                    height: 2,
                },
                orientation: 5,
                half_size: true,
                use_camera_white_balance: true,
                use_camera_matrix: true,
                use_auto_brightness: false,
                use_exposure_correction: false,
                brightness: 1.25,
                maximum_adjustment_threshold: 0.125,
                output_bits_per_channel: 16,
                demosaic_quality: 3,
                output_color: 1,
                gamma_inverse_power: 1.0,
                gamma_linear_toe_slope: 1.0,
                declared_dng_opcode_list_bytes: [11, 22, 33],
                process_warnings: 0x1024,
            }
        );
        assert!(recorded.recorded());
        assert!(recorded.uses_current_schema());

        let serialized = serde_json::to_vec(&recorded).expect("serialize receipt");
        assert_eq!(
            serde_json::from_slice::<RawDevelopmentReceipt>(&serialized)
                .expect("deserialize receipt"),
            recorded,
            "the bridge's fixed receipt remains serializable without losing provenance"
        );
    }

    const TINY_GRAYSCALE_JPEG: &[u8] = &[
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02,
        0x02, 0x03, 0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05, 0x05, 0x04, 0x04,
        0x05, 0x0a, 0x07, 0x07, 0x06, 0x08, 0x0c, 0x0a, 0x0c, 0x0c, 0x0b, 0x0a, 0x0b, 0x0b, 0x0d,
        0x0e, 0x12, 0x10, 0x0d, 0x0e, 0x11, 0x0e, 0x0b, 0x0b, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14,
        0x15, 0x15, 0x15, 0x0c, 0x0f, 0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xff,
        0xc0, 0x00, 0x0b, 0x08, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00,
        0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x09, 0xff, 0xc4, 0x00, 0x1d, 0x10, 0x00, 0x02, 0x01, 0x04, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x06, 0x03, 0x04,
        0x05, 0x07, 0x00, 0x12, 0x62, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00,
        0x41, 0xe2, 0xfa, 0x1b, 0x59, 0xd3, 0x8d, 0x62, 0x55, 0x75, 0xdc, 0x4d, 0x55, 0x6d, 0x28,
        0x80, 0xa3, 0x09, 0x6c, 0x00, 0x1d, 0x07, 0x8e, 0x7f, 0xff, 0xd9,
    ];

    #[test]
    fn jpeg_display_luma_crosses_as_a_bounded_deterministic_plane() {
        let first = decode_jpeg_display_luma(TINY_GRAYSCALE_JPEG, 512)
            .expect("decode valid tiny JPEG display proxy");
        let second = decode_jpeg_display_luma(TINY_GRAYSCALE_JPEG, 512)
            .expect("repeat valid tiny JPEG display proxy");
        assert_eq!((first.width, first.height, first.stride), (2, 2, 2));
        assert_eq!(first.samples.len(), 4);
        assert_eq!(first, second);
        assert_eq!(
            first
                .samples
                .iter()
                .map(|sample| sample.to_bits())
                .collect::<Vec<_>>(),
            [1_023_443_073, 1_050_319_515, 1_061_405_636, 1_065_353_216],
            "the version-pinned JPEG dependency must retain golden sample values"
        );
        assert!(
            first
                .samples
                .iter()
                .all(|value| (0.0..=1.0).contains(value))
        );
        assert!(first.samples[0] < first.samples[3]);
        assert_eq!(
            first.preprocessing_version,
            format!("{JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX}:max-edge-512")
        );
    }

    #[test]
    fn jpeg_display_luma_rejects_corruption_limits_and_invalid_bounds() {
        for invalid_edge in [0, MAX_JPEG_DISPLAY_LUMA_EDGE + 1] {
            assert!(matches!(
                decode_jpeg_display_luma(&[], invalid_edge),
                Err(BridgeError::InvalidDisplayLumaRequest(_))
            ));
        }

        for corrupt in [
            vec![0xff, 0xd8, 0xff],
            TINY_GRAYSCALE_JPEG[..TINY_GRAYSCALE_JPEG.len() - 2].to_vec(),
        ] {
            assert!(matches!(
                decode_jpeg_display_luma(&corrupt, 512),
                Err(BridgeError::Decoder(_))
            ));
        }

        let mut oversized = TINY_GRAYSCALE_JPEG.to_vec();
        let sof = oversized
            .windows(2)
            .position(|window| window == [0xff, 0xc0])
            .expect("fixture has baseline SOF");
        oversized[sof + 5..sof + 9].copy_from_slice(&[0x4e, 0x20, 0x4e, 0x20]);
        let error = decode_jpeg_display_luma(&oversized, 512)
            .expect_err("oversized dimensions fail before entropy decode");
        assert!(matches!(error, BridgeError::Decoder(_)));
        assert!(
            error
                .to_string()
                .contains("dimensions or pixel count exceed limits")
        );
    }

    #[test]
    fn basic_edit_defaults_are_a_bounded_neutral_recipe() {
        let request = EditedProxyRequest::default();
        assert_eq!(request.edits, BasicEditParameters::default());
        assert_eq!(request.max_edge, 2_048);
        assert_eq!(request.jpeg_quality, 95);
        request.validate().expect("neutral recipe is valid");
    }

    #[test]
    fn basic_compatibility_builds_the_expected_typed_plan() {
        let plan = basic_adjustment_render_plan(BasicEditParameters::default())
            .expect("neutral basic edits compile");
        assert_eq!(plan.nodes.len(), 4);
        assert!(matches!(
            plan.nodes[0].operation,
            AdjustmentRenderOperation::Exposure { stops: 0.0 }
        ));
        assert!(matches!(
            plan.nodes[1].operation,
            AdjustmentRenderOperation::Contrast {
                factor: 1.0,
                pivot: 0.18
            }
        ));
        assert!(matches!(
            plan.nodes[2].operation,
            AdjustmentRenderOperation::RgbWhiteBalance {
                temperature: 0.0,
                tint: 0.0
            }
        ));
        assert!(matches!(
            plan.nodes[3].operation,
            AdjustmentRenderOperation::Saturation { factor: 1.0 }
        ));
    }

    #[test]
    fn typed_plan_rejects_duplicate_ids_and_malformed_curves() {
        let node = |node_id: &str, operation| AdjustmentRenderNode {
            node_id: node_id.to_owned(),
            parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
            implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
            enabled: true,
            operation,
        };
        let duplicate = AdjustmentRenderPlan {
            nodes: vec![
                node("same", AdjustmentRenderOperation::Exposure { stops: 0.0 }),
                node(
                    "same",
                    AdjustmentRenderOperation::Saturation { factor: 1.0 },
                ),
            ],
        };
        assert!(matches!(
            duplicate.validate(),
            Err(BridgeError::InvalidEditRequest(_))
        ));

        for points in [
            vec![
                ToneCurvePoint { x: 0.1, y: 0.0 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ],
            vec![
                ToneCurvePoint { x: 0.0, y: 0.0 },
                ToneCurvePoint { x: 0.0, y: 0.5 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ],
        ] {
            let malformed = AdjustmentRenderPlan {
                nodes: vec![node(
                    "curve",
                    AdjustmentRenderOperation::ToneCurve { points },
                )],
            };
            assert!(matches!(
                malformed.validate(),
                Err(BridgeError::InvalidEditRequest(_))
            ));
        }

        for operation in [
            AdjustmentRenderOperation::Exposure { stops: -2_000.0 },
            AdjustmentRenderOperation::Contrast {
                factor: -0.1,
                pivot: 0.18,
            },
            AdjustmentRenderOperation::ToneCurve {
                points: vec![
                    ToneCurvePoint { x: 0.0, y: 0.0 },
                    ToneCurvePoint {
                        x: f64::MIN_POSITIVE,
                        y: f64::MAX,
                    },
                    ToneCurvePoint { x: 1.0, y: 1.0 },
                ],
            },
            AdjustmentRenderOperation::RgbWhiteBalance {
                temperature: 0.0,
                tint: 2.0,
            },
            AdjustmentRenderOperation::Saturation { factor: -0.1 },
        ] {
            let invalid = AdjustmentRenderPlan {
                nodes: vec![node("invalid", operation)],
            };
            assert!(matches!(
                invalid.validate(),
                Err(BridgeError::InvalidEditRequest(_))
            ));
        }
    }

    #[test]
    #[allow(clippy::float_cmp)] // FFI flattening is an exact in-memory contract.
    fn extended_plan_validates_and_flattens_the_stable_ffi_contract() {
        let perceptual = PerceptualColorParameters {
            vibrance: 0.2,
            hue_shifts: [0.1; COLOR_MIXER_BAND_COUNT],
            saturation: [-0.2; COLOR_MIXER_BAND_COUNT],
            lightness: [0.3; COLOR_MIXER_BAND_COUNT],
            color_range: ColorRangeParameters {
                enabled: true,
                center_hue_degrees: 45.0,
                width_degrees: 60.0,
                softness: 0.4,
                hue_shift_degrees: 15.0,
                saturation: 0.5,
                lightness: -0.6,
            },
            additional_color_ranges: Vec::new(),
        };
        let plan = AdjustmentRenderPlan {
            nodes: vec![
                AdjustmentRenderNode {
                    node_id: "selective-tone".to_owned(),
                    parameter_schema_version: SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION,
                    implementation_version: SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION,
                    enabled: true,
                    operation: AdjustmentRenderOperation::SelectiveTone {
                        parameters: SelectiveToneParameters {
                            highlights: -1.0,
                            shadows: -0.25,
                            whites: 0.5,
                            blacks: 1.0,
                        },
                    },
                },
                AdjustmentRenderNode {
                    node_id: "perceptual-color".to_owned(),
                    parameter_schema_version: PERCEPTUAL_COLOR_V2_PARAMETER_SCHEMA_VERSION,
                    implementation_version: PERCEPTUAL_COLOR_V2_IMPLEMENTATION_VERSION,
                    enabled: true,
                    operation: AdjustmentRenderOperation::PerceptualColor {
                        parameters: Box::new(perceptual),
                    },
                },
                AdjustmentRenderNode {
                    node_id: "technical-detail".to_owned(),
                    parameter_schema_version: TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION,
                    implementation_version: TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION,
                    enabled: true,
                    operation: AdjustmentRenderOperation::Sharpen {
                        parameters: Box::new(SharpenParameters {
                            amount: 1.25,
                            radius: 2.5,
                            threshold: 0.15,
                            masking: 0.75,
                            ..SharpenParameters::default()
                        }),
                    },
                },
            ],
        };

        plan.validate().expect("extended plan is valid");
        let selective_ffi = ffi_render_node(&plan.nodes[0]);
        assert!(matches!(
            selective_ffi.operation,
            ffi::FfiAdjustmentOperation::SelectiveTone
        ));
        assert_eq!(selective_ffi.parameters, [-1.0, -0.25, 0.5, 1.0]);

        let perceptual_ffi = ffi_render_node(&plan.nodes[1]);
        assert!(matches!(
            perceptual_ffi.operation,
            ffi::FfiAdjustmentOperation::PerceptualColor
        ));
        assert_eq!(perceptual_ffi.parameters.len(), 32);
        assert_eq!(perceptual_ffi.parameter_group_lengths, [0]);
        assert_eq!(perceptual_ffi.parameters[0], 0.2);
        assert_eq!(&perceptual_ffi.parameters[1..9], &[0.1; 8]);
        assert_eq!(&perceptual_ffi.parameters[9..17], &[-0.2; 8]);
        assert_eq!(&perceptual_ffi.parameters[17..25], &[0.3; 8]);
        assert_eq!(
            &perceptual_ffi.parameters[25..],
            &[1.0, 45.0, 60.0, 0.4, 15.0, 0.5, -0.6]
        );

        let sharpen_ffi = ffi_render_node(&plan.nodes[2]);
        assert!(matches!(
            sharpen_ffi.operation,
            ffi::FfiAdjustmentOperation::Sharpen
        ));
        assert_eq!(sharpen_ffi.parameters.len(), 33);
        assert_eq!(&sharpen_ffi.parameters[..4], &[1.25, 2.5, 0.15, 0.75]);
        assert_eq!(
            &sharpen_ffi.parameters[4..],
            &[
                0.0, 0.5, 0.0, 0.0, 0.0, 270.0, 340.0, 0.0, 100.0, 165.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                0.0, 0.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.5, 0.5, 0.0, 0.5, 0.0, 0.5, 0.0,
            ]
        );
    }

    #[test]
    #[allow(clippy::float_cmp)] // FFI flattening is an exact in-memory contract.
    fn smooth_rgb_tone_curve_uses_explicit_group_lengths_and_v2_contract() {
        let curves = SmoothRgbToneCurve {
            master: vec![
                ToneCurvePoint { x: 0.0, y: 0.02 },
                ToneCurvePoint { x: 0.5, y: 0.62 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ],
            red: SmoothRgbToneCurve::default().red,
            green: vec![
                ToneCurvePoint { x: 0.0, y: 0.0 },
                ToneCurvePoint { x: 0.25, y: 0.2 },
                ToneCurvePoint { x: 0.8, y: 0.9 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ],
            blue: SmoothRgbToneCurve::default().blue,
        };
        let node = AdjustmentRenderNode {
            node_id: "smooth-rgb-curve".to_owned(),
            parameter_schema_version: SMOOTH_RGB_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
            implementation_version: SMOOTH_RGB_TONE_CURVE_IMPLEMENTATION_VERSION,
            enabled: true,
            operation: AdjustmentRenderOperation::SmoothRgbToneCurve {
                curves: Box::new(curves),
            },
        };
        AdjustmentRenderPlan {
            nodes: vec![node.clone()],
        }
        .validate()
        .expect("smooth RGB Tone Curve v2 contract");

        let encoded = ffi_render_node(&node);
        assert!(matches!(
            encoded.operation,
            ffi::FfiAdjustmentOperation::SmoothRgbToneCurve
        ));
        assert_eq!(encoded.parameter_group_lengths, [3, 2, 4, 2]);
        assert_eq!(encoded.parameters.len(), 22);
        assert_eq!(&encoded.parameters[..6], &[0.0, 0.02, 0.5, 0.62, 1.0, 1.0]);

        for (parameter_schema_version, implementation_version) in [
            (
                ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                SMOOTH_RGB_TONE_CURVE_IMPLEMENTATION_VERSION,
            ),
            (
                SMOOTH_RGB_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
                ADJUSTMENT_IMPLEMENTATION_VERSION,
            ),
        ] {
            let mut mixed = node.clone();
            mixed.parameter_schema_version = parameter_schema_version;
            mixed.implementation_version = implementation_version;
            assert!(matches!(
                AdjustmentRenderPlan { nodes: vec![mixed] }.validate(),
                Err(BridgeError::InvalidEditRequest(
                    "adjustment node uses an unsupported schema or implementation version"
                ))
            ));
        }

        let mut malformed = SmoothRgbToneCurve::default();
        malformed.blue.clear();
        let malformed = AdjustmentRenderPlan {
            nodes: vec![AdjustmentRenderNode {
                node_id: "malformed-smooth-rgb-curve".to_owned(),
                parameter_schema_version: SMOOTH_RGB_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
                implementation_version: SMOOTH_RGB_TONE_CURVE_IMPLEMENTATION_VERSION,
                enabled: true,
                operation: AdjustmentRenderOperation::SmoothRgbToneCurve {
                    curves: Box::new(malformed),
                },
            }],
        };
        assert!(matches!(
            malformed.validate(),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }

    #[test]
    fn extended_plan_rejects_non_finite_and_out_of_range_values() {
        let node = |operation| AdjustmentRenderPlan {
            nodes: vec![AdjustmentRenderNode {
                node_id: "invalid-extended-control".to_owned(),
                parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                enabled: true,
                operation,
            }],
        };

        for parameters in [
            SelectiveToneParameters {
                highlights: 1.01,
                ..SelectiveToneParameters::default()
            },
            SelectiveToneParameters {
                shadows: f64::NAN,
                ..SelectiveToneParameters::default()
            },
        ] {
            assert!(matches!(
                node(AdjustmentRenderOperation::SelectiveTone { parameters }).validate(),
                Err(BridgeError::InvalidEditRequest(_))
            ));
        }

        let mut invalid_mixer = PerceptualColorParameters::default();
        invalid_mixer.hue_shifts[3] = -1.01;
        let mut invalid_disabled_range = PerceptualColorParameters::default();
        invalid_disabled_range.color_range.enabled = false;
        invalid_disabled_range.color_range.width_degrees = 0.0;
        for parameters in [invalid_mixer, invalid_disabled_range] {
            assert!(matches!(
                node(AdjustmentRenderOperation::PerceptualColor {
                    parameters: Box::new(parameters),
                })
                .validate(),
                Err(BridgeError::InvalidEditRequest(_))
            ));
        }

        for parameters in [
            SharpenParameters {
                amount: 2.01,
                ..SharpenParameters::default()
            },
            SharpenParameters {
                radius: 0.09,
                ..SharpenParameters::default()
            },
            SharpenParameters {
                threshold: 1.01,
                ..SharpenParameters::default()
            },
            SharpenParameters {
                masking: f64::NAN,
                ..SharpenParameters::default()
            },
        ] {
            assert!(matches!(
                node(AdjustmentRenderOperation::Sharpen {
                    parameters: Box::new(parameters),
                })
                .validate(),
                Err(BridgeError::InvalidEditRequest(_))
            ));
        }
    }

    #[test]
    fn typed_plan_rejects_unsupported_contract_versions() {
        for (parameter_schema_version, implementation_version) in [
            (
                ADJUSTMENT_PARAMETER_SCHEMA_VERSION + 1,
                ADJUSTMENT_IMPLEMENTATION_VERSION,
            ),
            (
                ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                ADJUSTMENT_IMPLEMENTATION_VERSION + 1,
            ),
        ] {
            let plan = AdjustmentRenderPlan {
                nodes: vec![AdjustmentRenderNode {
                    node_id: "future-exposure".to_owned(),
                    parameter_schema_version,
                    implementation_version,
                    enabled: true,
                    operation: AdjustmentRenderOperation::Exposure { stops: 0.0 },
                }],
            };

            assert!(matches!(
                plan.validate(),
                Err(BridgeError::InvalidEditRequest(
                    "adjustment node uses an unsupported schema or implementation version"
                ))
            ));
        }

        let one_pass_v2 = AdjustmentRenderPlan {
            nodes: vec![AdjustmentRenderNode {
                node_id: "discarded-selective-tone-v2".to_owned(),
                parameter_schema_version: 2,
                implementation_version: 2,
                enabled: true,
                operation: AdjustmentRenderOperation::SelectiveTone {
                    parameters: SelectiveToneParameters {
                        shadows: 0.5,
                        ..SelectiveToneParameters::default()
                    },
                },
            }],
        };
        assert!(matches!(
            one_pass_v2.validate(),
            Err(BridgeError::InvalidEditRequest(
                "adjustment node uses an unsupported schema or implementation version"
            ))
        ));
    }

    #[test]
    fn warm_edit_session_is_send_sync_and_bounded_before_raw_io() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<LibRawEditPreviewSession>();
        assert_send_sync::<PhotoEditPreviewSession>();

        assert_eq!(
            std::any::TypeId::of::<PhotoEditPreviewSession>(),
            std::any::TypeId::of::<LibRawEditPreviewSession>(),
            "the source-neutral API must not add a second session allocation or threading model"
        );

        for max_edge in [0, MAX_WARM_EDIT_PREVIEW_EDGE + 1] {
            let error = PhotoEditPreviewSession::open(
                Path::new("fixture-that-must-not-be-opened.raw"),
                max_edge,
            )
            .expect_err("invalid warm bound must fail before opening the source");
            assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
        }
    }

    #[test]
    fn photo_router_reference_proxy_rejects_invalid_requests_before_source_io() {
        for (max_edge, jpeg_quality) in [(0, 82), (16_385, 82), (1_024, 0), (1_024, 101)] {
            let error = render_photo_reference_proxy(
                Path::new("fixture-that-must-not-be-opened.photo"),
                max_edge,
                jpeg_quality,
            )
            .expect_err("invalid generic proxy request must fail before opening the source");
            assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
        }
    }

    #[test]
    fn photo_router_reports_mandatory_jpeg_raster_support() {
        let extensions = photo_supported_raster_extensions();
        assert!(extensions.contains(&"jpg".to_owned()));
        assert!(extensions.contains(&"jpeg".to_owned()));
    }

    fn valid_ffi_edit_preview_analysis() -> ffi::FfiEditPreviewAnalysis {
        let histogram = || {
            let mut bins = vec![0_u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT];
            bins[0] = 2;
            bins
        };
        ffi::FfiEditPreviewAnalysis {
            version: EDIT_PREVIEW_ANALYSIS_VERSION.to_owned(),
            sample_dimensions: ffi::FfiDimensions {
                width: 2,
                height: 1,
            },
            red: histogram(),
            green: histogram(),
            blue: histogram(),
            luma: histogram(),
            below_zero_samples: vec![0, 0, 0],
            above_one_samples: vec![1, 0, 0],
            pixel_count: 2,
            shadow_clipped_pixels: 0,
            highlight_clipped_pixels: 1,
        }
    }

    fn valid_edit_preview_proxy() -> shadow_domain::ProxyPayload {
        shadow_domain::ProxyPayload {
            dimensions: ImageDimensions {
                width: 2,
                height: 1,
            },
            codec: PreviewCodec::Jpeg,
            bits_per_channel: 8,
            channels: 3,
            bytes: vec![0xff, 0xd8, 0xff, 0xd9],
        }
    }

    #[test]
    fn edit_preview_analysis_validation_fails_closed() {
        let proxy_dimensions = ImageDimensions {
            width: 2,
            height: 1,
        };
        let valid =
            validate_edit_preview_analysis(valid_ffi_edit_preview_analysis(), proxy_dimensions)
                .expect("valid analysis contract");
        assert_eq!(valid.version, EDIT_PREVIEW_ANALYSIS_VERSION);
        assert_eq!(valid.pixel_count, 2);
        assert_eq!(valid.red.iter().sum::<u64>(), 2);
        assert_eq!(valid.highlight_clipped_pixels, 1);

        let mut wrong_version = valid_ffi_edit_preview_analysis();
        wrong_version.version.push_str(":future");
        assert!(matches!(
            validate_edit_preview_analysis(wrong_version, proxy_dimensions),
            Err(BridgeError::InvalidEditPreviewOutput(_))
        ));

        let wrong_proxy_dimensions = ImageDimensions {
            width: 1,
            height: 2,
        };
        assert!(matches!(
            validate_edit_preview_analysis(
                valid_ffi_edit_preview_analysis(),
                wrong_proxy_dimensions
            ),
            Err(BridgeError::InvalidEditPreviewOutput(_))
        ));

        let mut short_histogram = valid_ffi_edit_preview_analysis();
        short_histogram.luma.pop();
        assert!(matches!(
            validate_edit_preview_analysis(short_histogram, proxy_dimensions),
            Err(BridgeError::InvalidEditPreviewOutput(_))
        ));

        let mut wrong_histogram_sum = valid_ffi_edit_preview_analysis();
        wrong_histogram_sum.blue[0] = 1;
        assert!(matches!(
            validate_edit_preview_analysis(wrong_histogram_sum, proxy_dimensions),
            Err(BridgeError::InvalidEditPreviewOutput(_))
        ));

        let mut impossible_channel_counts = valid_ffi_edit_preview_analysis();
        impossible_channel_counts.below_zero_samples = vec![2, 0, 0];
        impossible_channel_counts.above_one_samples = vec![1, 0, 0];
        impossible_channel_counts.shadow_clipped_pixels = 2;
        assert!(matches!(
            validate_edit_preview_analysis(impossible_channel_counts, proxy_dimensions),
            Err(BridgeError::InvalidEditPreviewOutput(_))
        ));

        let mut impossible_any_channel_count = valid_ffi_edit_preview_analysis();
        impossible_any_channel_count.highlight_clipped_pixels = 2;
        assert!(matches!(
            validate_edit_preview_analysis(impossible_any_channel_count, proxy_dimensions),
            Err(BridgeError::InvalidEditPreviewOutput(_))
        ));

        validate_analyzed_edit_preview(
            valid_edit_preview_proxy(),
            valid_ffi_edit_preview_analysis(),
            proxy_dimensions,
        )
        .expect("matching analyzed proxy contract");

        let paired_wrong_dimensions = ImageDimensions {
            width: 1,
            height: 2,
        };
        let mut wrong_proxy = valid_edit_preview_proxy();
        wrong_proxy.dimensions = paired_wrong_dimensions;
        let mut matching_wrong_analysis = valid_ffi_edit_preview_analysis();
        matching_wrong_analysis.sample_dimensions = ffi::FfiDimensions {
            width: paired_wrong_dimensions.width,
            height: paired_wrong_dimensions.height,
        };
        assert!(matches!(
            validate_analyzed_edit_preview(wrong_proxy, matching_wrong_analysis, proxy_dimensions,),
            Err(BridgeError::InvalidEditPreviewOutput(_))
        ));

        for mutate in [
            |proxy: &mut shadow_domain::ProxyPayload| proxy.codec = PreviewCodec::Bitmap,
            |proxy: &mut shadow_domain::ProxyPayload| proxy.bits_per_channel = 16,
            |proxy: &mut shadow_domain::ProxyPayload| proxy.channels = 4,
            |proxy: &mut shadow_domain::ProxyPayload| proxy.bytes.clear(),
        ] {
            let mut proxy = valid_edit_preview_proxy();
            mutate(&mut proxy);
            assert!(matches!(
                validate_analyzed_edit_preview(
                    proxy,
                    valid_ffi_edit_preview_analysis(),
                    proxy_dimensions,
                ),
                Err(BridgeError::InvalidEditPreviewOutput(_))
            ));
        }
    }

    #[test]
    fn full_edit_detail_contract_is_send_sync_and_rejects_invalid_rectangles_locally() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<LibRawEditDetailSession>();
        assert_send_sync::<PhotoEditDetailSession>();
        assert_eq!(
            std::any::TypeId::of::<PhotoEditDetailSession>(),
            std::any::TypeId::of::<LibRawEditDetailSession>(),
            "the source-neutral detail API must retain the existing bounded session type"
        );
        assert_eq!(MAX_EDIT_DETAIL_TILE_SIDE, 1_024);
        assert_eq!(MAX_EDIT_DETAIL_RETAINED_BYTES, 512 * 1_024 * 1_024);

        let dimensions = ImageDimensions {
            width: 4_000,
            height: 3_000,
        };
        for rect in [
            DetailTileRect {
                x: 0,
                y: 0,
                width: 0,
                height: 1,
            },
            DetailTileRect {
                x: 0,
                y: 0,
                width: MAX_EDIT_DETAIL_TILE_SIDE + 1,
                height: 1,
            },
            DetailTileRect {
                x: dimensions.width,
                y: 0,
                width: 1,
                height: 1,
            },
            DetailTileRect {
                x: dimensions.width - 1,
                y: 0,
                width: 2,
                height: 1,
            },
            DetailTileRect {
                x: u32::MAX,
                y: 0,
                width: 1,
                height: 1,
            },
        ] {
            assert!(matches!(
                DetailTileRequest { rect }.validate(dimensions),
                Err(BridgeError::InvalidEditRequest(_))
            ));
        }
        DetailTileRequest {
            rect: DetailTileRect {
                x: dimensions.width - 1_024,
                y: dimensions.height - 1_024,
                width: 1_024,
                height: 1_024,
            },
        }
        .validate(dimensions)
        .expect("maximum in-bounds detail tile is valid without opening a RAW");
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
                    white_balance_temperature: 2.0,
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
    #[ignore = "requires SHADOW_TEST_RAW_FOLDER to contain local RAW fixtures"]
    #[allow(clippy::too_many_lines)] // Keeps the end-to-end local fixture contract in one test.
    fn real_raw_folder_smoke_matrix() {
        fn collect_raws(directory: &Path, paths: &mut Vec<std::path::PathBuf>) {
            let entries = std::fs::read_dir(directory).unwrap_or_else(|error| {
                panic!("read RAW fixture directory {directory:?}: {error}")
            });
            for entry in entries {
                let entry = entry.unwrap_or_else(|error| panic!("read RAW fixture entry: {error}"));
                let path = entry.path();
                if path.is_dir() {
                    collect_raws(&path, paths);
                    continue;
                }
                let extension = path
                    .extension()
                    .and_then(std::ffi::OsStr::to_str)
                    .unwrap_or_default()
                    .to_ascii_lowercase();
                if matches!(
                    extension.as_str(),
                    "3fr"
                        | "arw"
                        | "cr2"
                        | "cr3"
                        | "dng"
                        | "erf"
                        | "fff"
                        | "iiq"
                        | "kdc"
                        | "mef"
                        | "mos"
                        | "mrw"
                        | "nef"
                        | "nrw"
                        | "orf"
                        | "pef"
                        | "raf"
                        | "raw"
                        | "rw2"
                        | "rwl"
                        | "sr2"
                        | "srf"
                        | "srw"
                ) {
                    paths.push(path);
                }
            }
        }

        let folder = std::env::var_os("SHADOW_TEST_RAW_FOLDER")
            .expect("SHADOW_TEST_RAW_FOLDER must identify a fixture directory");
        let folder = Path::new(&folder);
        let mut paths = Vec::new();
        collect_raws(folder, &mut paths);
        paths.sort();
        assert!(
            !paths.is_empty(),
            "RAW fixture directory contains no supported files"
        );

        let mut failures = Vec::new();
        let mut passed = 0_usize;
        let mut preview_only = 0_usize;
        for path in paths {
            let result = (|| -> Result<(String, bool), String> {
                const FULL_DECODE_UNAVAILABLE: &str = "mosaic/reference RGB unavailable";
                const CAPABILITY_MISMATCH: &str = "mosaic and reference RGB capabilities disagree";
                let snapshot =
                    inspect_libraw(&path).map_err(|error| format!("inspect: {error}"))?;
                if snapshot.provider.id != "libraw" {
                    return Err(format!("unexpected provider {}", snapshot.provider.id));
                }
                if !snapshot.capabilities.metadata.is_available() {
                    return Err("metadata unavailable".to_owned());
                }
                if snapshot.metadata.raw_dimensions.pixel_count() == 0 {
                    return Err("invalid RAW dimensions".to_owned());
                }

                let preview = extract_best_libraw_preview(&path)
                    .map_err(|error| format!("extract preview: {error}"))?
                    .map(|preview| {
                        if preview.descriptor.dimensions.pixel_count() == 0
                            || preview.bytes.is_empty()
                        {
                            return Err("invalid embedded preview".to_owned());
                        }
                        Ok(preview)
                    })
                    .transpose()?;

                let mosaic_available = snapshot.capabilities.mosaic.is_available();
                let reference_rgb_available = snapshot.capabilities.reference_rgb.is_available();
                if mosaic_available != reference_rgb_available {
                    return Err(CAPABILITY_MISMATCH.to_owned());
                }

                let profile_count = query_libraw_optics_profiles(&path)
                    .map_err(|error| format!("query Lensfun profiles: {error}"))?
                    .len();
                let summary = format!(
                    "{} {} · {}x{} · {profile_count} compatible optical profiles",
                    snapshot.metadata.normalized_make,
                    snapshot.metadata.normalized_model,
                    snapshot.metadata.raw_dimensions.width,
                    snapshot.metadata.raw_dimensions.height,
                );
                if !mosaic_available {
                    if preview.is_none() {
                        return Err(format!("{FULL_DECODE_UNAVAILABLE}; no embedded preview"));
                    }
                    return Ok((format!("{summary} · embedded-preview fallback"), true));
                }

                let proxy = render_libraw_reference_proxy(&path, 1_024, 82)
                    .map_err(|error| format!("render reference proxy: {error}"))?;
                if proxy.codec != PreviewCodec::Jpeg {
                    return Err(format!("unexpected proxy codec: {:?}", proxy.codec));
                }
                if proxy.dimensions.width.max(proxy.dimensions.height) > 1_024
                    || proxy.dimensions.pixel_count() == 0
                    || proxy.bytes.is_empty()
                {
                    return Err("invalid bounded reference proxy".to_owned());
                }

                Ok((summary, false))
            })();

            match result {
                Ok((summary, used_preview_only)) => {
                    passed += 1;
                    if used_preview_only {
                        preview_only += 1;
                        eprintln!("RAW smoke preview-only: {} · {summary}", path.display());
                    } else {
                        eprintln!("RAW smoke ok: {} · {summary}", path.display());
                    }
                }
                Err(error) => {
                    eprintln!("RAW smoke failed: {} · {error}", path.display());
                    failures.push(format!("{} · {error}", path.display()));
                }
            }
        }

        assert!(
            failures.is_empty(),
            "RAW smoke matrix: {passed} passed ({preview_only} preview-only), {} failed:\n{}",
            failures.len(),
            failures.join("\n")
        );
        eprintln!(
            "RAW smoke matrix passed: {passed} files · {preview_only} preview-only fallbacks"
        );
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
    #[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
    fn real_dng_photo_router_entries_cross_the_bridge() {
        let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));

        let snapshot = inspect_photo(&path).expect("inspect local DNG through photo router");
        assert_eq!(snapshot.provider.id, "shadow-photo-router");
        assert_eq!(snapshot.provider.version, photo_provider_version());
        assert!(snapshot.capabilities.metadata.is_available());
        assert!(snapshot.metadata.raw_dimensions.pixel_count() > 0);

        let _profiles = query_photo_optics_profiles(&path)
            .expect("query local DNG optical profiles through router");
        let _preview = extract_best_photo_preview(&path)
            .expect("extract local DNG embedded preview through router");

        let proxy = render_photo_reference_proxy(&path, 1_024, 82)
            .expect("render local DNG reference proxy through router");
        assert_eq!(proxy.codec, PreviewCodec::Jpeg);
        assert!(proxy.dimensions.width.max(proxy.dimensions.height) <= 1_024);
        assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
        assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));

        let preview = PhotoEditPreviewSession::open(&path, 1_024)
            .expect("prepare generic local DNG preview session");
        assert!(preview.raw_development_receipt().recorded());
        let detail =
            PhotoEditDetailSession::open(&path).expect("prepare generic local DNG detail session");
        assert!(detail.raw_development_receipt().recorded());
    }

    #[test]
    #[ignore = "requires SHADOW_TEST_JPEG to point at a local JPEG fixture"]
    fn real_jpeg_photo_router_entries_cross_the_bridge() {
        let path = PathBuf::from(std::env::var_os("SHADOW_TEST_JPEG").expect("SHADOW_TEST_JPEG"));

        let snapshot = inspect_photo(&path).expect("inspect local JPEG through photo router");
        assert_eq!(snapshot.provider.id, "shadow-photo-router");
        assert_eq!(snapshot.provider.version, photo_provider_version());
        assert!(snapshot.capabilities.metadata.is_available());
        assert!(!snapshot.capabilities.mosaic.is_available());
        assert!(snapshot.capabilities.reference_rgb.is_available());
        assert!(snapshot.metadata.image_dimensions.pixel_count() > 0);

        assert!(
            query_photo_optics_profiles(&path)
                .expect("query local JPEG optical profiles through router")
                .is_empty()
        );
        assert!(
            extract_best_photo_preview(&path)
                .expect("extract local JPEG preview through router")
                .is_none()
        );

        let proxy = render_photo_reference_proxy(&path, 1_024, 82)
            .expect("render local JPEG reference proxy through router");
        assert_eq!(proxy.codec, PreviewCodec::Jpeg);
        assert!(proxy.dimensions.width.max(proxy.dimensions.height) <= 1_024);
        assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
        assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));

        let preview = PhotoEditPreviewSession::open(&path, 1_024)
            .expect("prepare generic local JPEG preview session");
        assert!(!preview.raw_development_receipt().recorded());
        let edited = preview
            .render(BasicEditParameters::default(), 82)
            .expect("render neutral generic JPEG preview session");
        assert_eq!(edited.codec, PreviewCodec::Jpeg);
        let detail =
            PhotoEditDetailSession::open(&path).expect("prepare generic local JPEG detail session");
        assert!(!detail.raw_development_receipt().recorded());
    }

    #[test]
    #[ignore = "requires SHADOW_TEST_HEIF to point at a local 8-bit SDR HEIF/HEIC fixture"]
    fn real_heif_photo_router_entries_cross_the_bridge() {
        let path = PathBuf::from(std::env::var_os("SHADOW_TEST_HEIF").expect("SHADOW_TEST_HEIF"));

        let extensions = photo_supported_raster_extensions();
        assert!(extensions.contains(&"heic".to_owned()));
        assert!(extensions.contains(&"heif".to_owned()));

        let snapshot = inspect_photo(&path).expect("inspect local HEIF through photo router");
        assert_eq!(snapshot.provider.id, "shadow-photo-router");
        assert_eq!(snapshot.provider.version, photo_provider_version());
        assert!(snapshot.capabilities.metadata.is_available());
        assert!(!snapshot.capabilities.mosaic.is_available());
        assert!(snapshot.capabilities.reference_rgb.is_available());
        assert!(snapshot.metadata.image_dimensions.pixel_count() > 0);
        assert_eq!(snapshot.metadata.orientation, 1);

        assert!(
            query_photo_optics_profiles(&path)
                .expect("query local HEIF optical profiles through router")
                .is_empty()
        );
        assert!(
            extract_best_photo_preview(&path)
                .expect("extract local HEIF preview through router")
                .is_none()
        );

        let proxy = render_photo_reference_proxy(&path, 1_024, 82)
            .expect("render local HEIF reference proxy through router");
        assert_eq!(proxy.codec, PreviewCodec::Jpeg);
        assert!(proxy.dimensions.width.max(proxy.dimensions.height) <= 1_024);
        assert!(proxy.bytes.starts_with(&[0xff, 0xd8]));
        assert!(proxy.bytes.ends_with(&[0xff, 0xd9]));

        let preview = PhotoEditPreviewSession::open(&path, 1_024)
            .expect("prepare generic local HEIF preview session");
        assert!(!preview.raw_development_receipt().recorded());
        let edited = preview
            .render(BasicEditParameters::default(), 82)
            .expect("render neutral generic HEIF preview session");
        assert_eq!(edited.codec, PreviewCodec::Jpeg);
        let detail =
            PhotoEditDetailSession::open(&path).expect("prepare generic local HEIF detail session");
        assert!(!detail.raw_development_receipt().recorded());
    }

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
    fn real_dng_raw_development_receipts_cross_all_prepared_handles() {
        let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));
        let handle = open_libraw(&path).expect("open local DNG");
        let decoder = handle.as_ref().expect("non-null decoder handle");

        let before_preparation = raw_development_receipt(decoder.raw_development_receipt());
        assert_eq!(before_preparation, RawDevelopmentReceipt::default());

        let preview_handle = decoder
            .prepare_edit_preview(1_024)
            .expect("prepare local DNG warm preview");
        let preview = preview_handle.as_ref().expect("non-null preview handle");
        let preview_receipt = raw_development_receipt(preview.raw_development_receipt());
        assert!(preview_receipt.recorded());
        assert!(preview_receipt.uses_current_schema());
        assert_eq!(preview_receipt.provider_id, "libraw");
        assert_eq!(
            raw_development_receipt(decoder.raw_development_receipt()),
            preview_receipt,
            "DecodeHandle reports the last source render it prepared"
        );

        let detail_handle = decoder
            .prepare_edit_detail()
            .expect("prepare local DNG full detail");
        let detail = detail_handle.as_ref().expect("non-null detail handle");
        let detail_receipt = raw_development_receipt(detail.raw_development_receipt());
        assert!(detail_receipt.recorded());
        assert!(detail_receipt.uses_current_schema());
        assert_eq!(detail_receipt.provider_id, "libraw");
        assert!(!detail_receipt.half_size);
        assert_eq!(
            raw_development_receipt(decoder.raw_development_receipt()),
            detail_receipt,
            "DecodeHandle updates its read-only receipt when a new source render is prepared"
        );
    }

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG to identify a camera present in Lensfun"]
    fn real_dng_enumerates_compatible_lensfun_profiles() {
        let path = std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG");
        let candidates =
            query_libraw_optics_profiles(Path::new(&path)).expect("query Lensfun candidates");
        assert!(!candidates.is_empty());
        assert!(candidates.iter().all(|candidate| {
            !candidate.camera_model.is_empty() && !candidate.lens_model.is_empty()
        }));
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
                    white_balance_temperature: 0.12,
                    white_balance_tint: 0.04,
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

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
    fn real_dng_warm_edit_session_renders_twice() {
        let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));
        std::thread::Builder::new()
            .name("small-edit-worker".to_owned())
            .stack_size(512 * 1_024)
            .spawn(move || {
                let session = LibRawEditPreviewSession::open(&path, 1_024)
                    .expect("prepare warm local DNG edit session on a small worker stack");
                let neutral = session
                    .render(BasicEditParameters::default(), 86)
                    .expect("render neutral warm preview");
                let neutral_plan = basic_adjustment_render_plan(BasicEditParameters::default())
                    .expect("build neutral typed plan");
                let neutral_from_plan = session
                    .render_plan(&neutral_plan, 86)
                    .expect("render neutral typed plan");
                let neutral_analyzed = session
                    .render_plan_with_analysis(&neutral_plan, 86)
                    .expect("render neutral typed plan with analysis");
                let adjusted = session
                    .render(
                        BasicEditParameters {
                            exposure_stops: 1.0,
                            contrast_factor: 1.1,
                            white_balance_temperature: 0.12,
                            white_balance_tint: 0.04,
                            saturation_factor: 1.15,
                        },
                        86,
                    )
                    .expect("render adjusted warm preview");
                let mut curved_plan = basic_adjustment_render_plan(BasicEditParameters::default())
                    .expect("build neutral typed plan");
                curved_plan.nodes.insert(
                    2,
                    AdjustmentRenderNode {
                        node_id: "test-tone-curve".to_owned(),
                        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                        enabled: true,
                        operation: AdjustmentRenderOperation::ToneCurve {
                            points: vec![
                                ToneCurvePoint { x: 0.0, y: 0.0 },
                                ToneCurvePoint { x: 0.5, y: 0.7 },
                                ToneCurvePoint { x: 1.0, y: 1.0 },
                            ],
                        },
                    },
                );
                let curved = session
                    .render_plan(&curved_plan, 86)
                    .expect("render typed Tone Curve plan");

                assert_eq!(session.dimensions(), neutral.dimensions);
                assert_eq!(neutral_from_plan.bytes, neutral.bytes);
                assert_eq!(neutral_analyzed.proxy.bytes, neutral.bytes);
                assert_eq!(
                    neutral_analyzed.analysis.sample_dimensions,
                    neutral.dimensions
                );
                assert_eq!(
                    neutral_analyzed.analysis.pixel_count,
                    neutral.dimensions.pixel_count()
                );
                for histogram in [
                    &neutral_analyzed.analysis.red,
                    &neutral_analyzed.analysis.green,
                    &neutral_analyzed.analysis.blue,
                    &neutral_analyzed.analysis.luma,
                ] {
                    assert_eq!(
                        histogram.iter().sum::<u64>(),
                        neutral_analyzed.analysis.pixel_count
                    );
                }
                assert_eq!(
                    neutral_analyzed.analysis.version,
                    EDIT_PREVIEW_ANALYSIS_VERSION
                );
                assert_eq!(adjusted.dimensions, neutral.dimensions);
                assert_eq!(curved.dimensions, neutral.dimensions);
                assert_eq!(neutral.codec, PreviewCodec::Jpeg);
                assert!(neutral.bytes.starts_with(&[0xff, 0xd8]));
                assert!(adjusted.bytes.ends_with(&[0xff, 0xd9]));
                assert_ne!(adjusted.bytes, neutral.bytes);
                assert_ne!(curved.bytes, neutral.bytes);
            })
            .expect("spawn small edit worker")
            .join()
            .expect("small edit worker did not panic");
    }

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
    fn real_dng_full_edit_detail_session_renders_deterministic_tiles() {
        let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));
        std::thread::Builder::new()
            .name("small-detail-worker".to_owned())
            .stack_size(512 * 1_024)
            .spawn(move || {
                let session = LibRawEditDetailSession::open(&path)
                    .expect("prepare full local DNG detail session on a small worker stack");
                let full = session.dimensions();
                assert!(full.width > 0 && full.height > 0);
                assert!((1..=MAX_EDIT_DETAIL_RETAINED_BYTES).contains(&session.retained_bytes()));
                let width = full.width.min(512);
                let height = full.height.min(512);
                let request = DetailTileRequest {
                    rect: DetailTileRect {
                        x: (full.width - width) / 2,
                        y: (full.height - height) / 2,
                        width,
                        height,
                    },
                };
                let neutral_plan = basic_adjustment_render_plan(BasicEditParameters::default())
                    .expect("build neutral detail plan");
                let first = session
                    .render_plan_tile(&neutral_plan, request)
                    .expect("render neutral full-resolution detail tile");
                let second = session
                    .render_plan_tile(&neutral_plan, request)
                    .expect("repeat neutral full-resolution detail tile");
                assert_eq!(first, second);
                assert_eq!(first.rect, request.rect);
                assert_eq!(first.full_dimensions, full);
                assert_eq!(first.row_stride_bytes, width * 3);
                assert_eq!(
                    first.bytes.len(),
                    usize::try_from(width * 3)
                        .unwrap()
                        .checked_mul(usize::try_from(height).unwrap())
                        .unwrap()
                );

                let adjusted_plan = basic_adjustment_render_plan(BasicEditParameters {
                    exposure_stops: 1.0,
                    ..BasicEditParameters::default()
                })
                .expect("build adjusted detail plan");
                let adjusted = session
                    .render_plan_tile(&adjusted_plan, request)
                    .expect("render adjusted full-resolution detail tile");
                assert_ne!(adjusted.bytes, first.bytes);
            })
            .expect("spawn small detail worker")
            .join()
            .expect("small detail worker did not panic");
    }
}
