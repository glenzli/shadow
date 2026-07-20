//! Safe, coarse-grained Rust access to Shadow's C++ image decoder providers.

use std::{
    collections::HashSet,
    path::{Path, PathBuf},
};

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
        ChannelGain,
        Saturation,
    }

    #[derive(Debug)]
    struct FfiAdjustmentNode {
        node_id: String,
        operation: FfiAdjustmentOperation,
        parameter_schema_version: u32,
        implementation_version: u32,
        enabled: bool,
        parameters: Vec<f64>,
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
        fn libraw_provider_version() -> String;
        fn decode_jpeg_display_luma(encoded: &[u8], max_edge: u32) -> Result<FfiDisplayLuma>;
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

/// Hard memory bound for the reusable float working proxy.
///
/// A square proxy at this edge consumes at most 192 MiB for interleaved RGB
/// float32. The intended UI values are 1600 and 2048.
pub const MAX_WARM_EDIT_PREVIEW_EDGE: u32 = 4_096;

/// Number of bins in every warm edit-preview display histogram.
pub const EDIT_PREVIEW_HISTOGRAM_BIN_COUNT: usize = 256;

/// Exact semantic contract for warm edit-preview analysis.
///
/// Histograms cover the complete uncompressed display-sRGB RGB8 warm proxy
/// immediately before JPEG encoding. Clipping counts inspect the edited
/// scene-linear samples before display clamping and use strict `< 0` and `> 1`
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

/// Current numeric contract understood by the C++ adjustment executor.
pub const ADJUSTMENT_PARAMETER_SCHEMA_VERSION: u32 = 1;
/// Current numeric implementation contract understood by the C++ executor.
pub const ADJUSTMENT_IMPLEMENTATION_VERSION: u32 = 1;
/// Hard bound for one linearized render plan crossing the language boundary.
pub const MAX_ADJUSTMENT_RENDER_NODES: usize = 256;
/// Hard bound for diagnostic node identities crossing the language boundary.
pub const MAX_ADJUSTMENT_NODE_ID_BYTES: usize = 256;
/// Mirrors the CPU reference Tone Curve bound without exposing a C++ type.
pub const MAX_TONE_CURVE_POINTS: usize = 256;

/// One point in the version-1 piecewise-linear Tone Curve contract.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ToneCurvePoint {
    pub x: f64,
    pub y: f64,
}

/// Typed pixel operation in execution order.
#[derive(Debug, Clone, PartialEq)]
pub enum AdjustmentRenderOperation {
    Exposure { stops: f64 },
    Contrast { factor: f64, pivot: f64 },
    ToneCurve { points: Vec<ToneCurvePoint> },
    ChannelGain { channel_gains: [f64; 3] },
    Saturation { factor: f64 },
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
/// pixel-local operations the current CPU reference backend can execute.
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
            if node.parameter_schema_version != ADJUSTMENT_PARAMETER_SCHEMA_VERSION
                || node.implementation_version != ADJUSTMENT_IMPLEMENTATION_VERSION
            {
                return Err(BridgeError::InvalidEditRequest(
                    "adjustment node uses an unsupported schema or implementation version",
                ));
            }
            validate_render_operation(&node.operation)?;
        }
        Ok(())
    }
}

#[allow(clippy::float_cmp)] // Tone Curve schema requires exact normalized endpoints.
fn validate_render_operation(operation: &AdjustmentRenderOperation) -> Result<(), BridgeError> {
    let finite = |value: f64| {
        if value.is_finite() {
            Ok(())
        } else {
            Err(BridgeError::InvalidEditRequest(
                "adjustment render parameters must be finite",
            ))
        }
    };
    match operation {
        AdjustmentRenderOperation::Exposure { stops } => {
            finite(*stops)?;
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
            finite(*factor)?;
            finite(*pivot)?;
            if *factor >= 0.0 && *pivot >= 0.0 {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "contrast factor and pivot must be non-negative",
                ))
            }
        }
        AdjustmentRenderOperation::ToneCurve { points } => {
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
                finite(point.x)?;
                finite(point.y)?;
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
        AdjustmentRenderOperation::ChannelGain { channel_gains } => {
            for gain in channel_gains {
                finite(*gain)?;
                if *gain <= 0.0 {
                    return Err(BridgeError::InvalidEditRequest(
                        "channel gains must be positive",
                    ));
                }
            }
            Ok(())
        }
        AdjustmentRenderOperation::Saturation { factor } => {
            finite(*factor)?;
            if *factor >= 0.0 {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "saturation factor must be non-negative",
                ))
            }
        }
    }
}

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
        for (index, gain) in self.channel_gains.into_iter().enumerate() {
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
                "basic-channel-gain",
                AdjustmentRenderOperation::ChannelGain {
                    channel_gains: edits.channel_gains,
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
            jpeg_quality: 88,
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

/// Returns the version string of the linked `LibRaw` provider without opening
/// an image.
pub fn libraw_provider_version() -> String {
    ffi::libraw_provider_version()
}

/// A reusable, bounded scene-linear working proxy for interactive edits.
///
/// [`Self::open`] performs the RAW render and scene-linear conversion once.
/// The resulting C++ handle retains only an immutable, max-edge-bounded RGB
/// float buffer; it does not retain a decoder or borrow the input path. The
/// handle is both [`Send`] and [`Sync`], and concurrent [`Self::render`] calls
/// use independent edit and JPEG buffers.
pub struct LibRawEditPreviewSession {
    handle: cxx::UniquePtr<ffi::EditPreviewHandle>,
    dimensions: ImageDimensions,
    max_edge: u32,
}

/// Transient analysis of one complete warm-proxy edit render.
///
/// The four histograms are derived from uncompressed display-sRGB RGB8 bytes
/// before JPEG encoding. Per-channel and any-channel clipping counts are
/// derived from the same render's scene-linear samples before output clamping;
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

/// Packed RGB8 sRGB bytes for one exact full-resolution rectangle.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RenderedDetailTile {
    pub rect: DetailTileRect,
    pub full_dimensions: ImageDimensions,
    pub row_stride_bytes: u32,
    pub bytes: Vec<u8>,
}

/// A reusable immutable full-resolution u16 sRGB source for 1:1 edit tiles.
///
/// Preparation performs one `LibRaw` reference render, retains no decoder, and fails when either
/// the metadata worst-case RGB allocation or the actual retained allocation exceeds 512 MiB.
/// Repeated tile renders convert and edit only the requested rectangle. The wrapper is
/// [`Send`] + [`Sync`], and concurrent renders own independent temporary buffers.
pub struct LibRawEditDetailSession {
    handle: cxx::UniquePtr<ffi::FullEditDetailHandle>,
    dimensions: ImageDimensions,
    retained_bytes: u64,
}

impl std::fmt::Debug for LibRawEditDetailSession {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LibRawEditDetailSession")
            .field("dimensions", &self.dimensions)
            .field("retained_bytes", &self.retained_bytes)
            .finish_non_exhaustive()
    }
}

impl std::fmt::Debug for LibRawEditPreviewSession {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LibRawEditPreviewSession")
            .field("dimensions", &self.dimensions)
            .field("max_edge", &self.max_edge)
            .finish_non_exhaustive()
    }
}

impl LibRawEditPreviewSession {
    /// Opens and decodes a RAW into a reusable scene-linear sRGB working proxy.
    ///
    /// `max_edge` must be in `1..=4096`; 1600 or 2048 are the intended UI
    /// values. The bound is checked before the input path is opened.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] before RAW I/O for an
    /// invalid bound, or a decoder error if preparation fails.
    pub fn open(path: &Path, max_edge: u32) -> Result<Self, BridgeError> {
        validate_warm_edit_max_edge(max_edge)?;
        let decode_handle = open_libraw(path)?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_preview(max_edge)?;
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let prepared_max_edge = prepared.max_edge();

        Ok(Self {
            handle,
            dimensions: prepared_dimensions,
            max_edge: prepared_max_edge,
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

    /// Re-runs only the fixed-order basic nodes and JPEG encoding.
    ///
    /// This method never opens or decodes the RAW. Since the prepared working
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
    /// This method never reopens or decodes the RAW.
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
    /// Opens and decodes a RAW into an immutable full-resolution u16 sRGB source.
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
        let decode_handle = open_libraw(path)?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_detail()?;
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let retained_bytes = prepared.retained_bytes();
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

    /// Executes a dependency-ordered typed plan against one exact full-resolution rectangle.
    ///
    /// Plan and rectangle shape/bounds are rejected in Rust before entering C++. The C++ kernel
    /// validates them again, converts only the crop to scene-linear float, executes the existing
    /// pixel-local nodes, and returns tightly packed RGB8 sRGB without compression or scaling.
    /// No RAW I/O occurs during this method.
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

fn ffi_render_node(node: &AdjustmentRenderNode) -> ffi::FfiAdjustmentNode {
    let (operation, parameters) = match &node.operation {
        AdjustmentRenderOperation::Exposure { stops } => {
            (ffi::FfiAdjustmentOperation::Exposure, vec![*stops])
        }
        AdjustmentRenderOperation::Contrast { factor, pivot } => {
            (ffi::FfiAdjustmentOperation::Contrast, vec![*factor, *pivot])
        }
        AdjustmentRenderOperation::ToneCurve { points } => (
            ffi::FfiAdjustmentOperation::ToneCurve,
            points.iter().flat_map(|point| [point.x, point.y]).collect(),
        ),
        AdjustmentRenderOperation::ChannelGain { channel_gains } => (
            ffi::FfiAdjustmentOperation::ChannelGain,
            channel_gains.to_vec(),
        ),
        AdjustmentRenderOperation::Saturation { factor } => {
            (ffi::FfiAdjustmentOperation::Saturation, vec![*factor])
        }
    };
    ffi::FfiAdjustmentNode {
        node_id: node.node_id.clone(),
        operation,
        parameter_schema_version: node.parameter_schema_version,
        implementation_version: node.implementation_version,
        enabled: node.enabled,
        parameters,
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
        assert_eq!(request.jpeg_quality, 88);
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
            AdjustmentRenderOperation::ChannelGain {
                channel_gains: [1.0, 1.0, 1.0]
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
            AdjustmentRenderOperation::ChannelGain {
                channel_gains: [1.0, 0.0, 1.0],
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
    }

    #[test]
    fn warm_edit_session_is_send_sync_and_bounded_before_raw_io() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<LibRawEditPreviewSession>();

        for max_edge in [0, MAX_WARM_EDIT_PREVIEW_EDGE + 1] {
            let error = LibRawEditPreviewSession::open(
                Path::new("fixture-that-must-not-be-opened.raw"),
                max_edge,
            )
            .expect_err("invalid warm bound must fail before opening the RAW");
            assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
        }
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
                            channel_gains: [1.05, 1.0, 0.95],
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
