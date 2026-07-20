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

    unsafe extern "C++" {
        include!("shadow/image/cxx_bridge.hpp");

        type DecodeHandle;
        type EditPreviewHandle;

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
        fn render_adjustment_plan(
            self: &DecodeHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiEncodedProxy>;
        fn prepare_edit_preview(
            self: &DecodeHandle,
            max_edge: u32,
        ) -> Result<UniquePtr<EditPreviewHandle>>;
        fn dimensions(self: &EditPreviewHandle) -> FfiDimensions;
        fn max_edge(self: &EditPreviewHandle) -> u32;
        fn render_adjustment_plan(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiEncodedProxy>;
    }
}

// SAFETY: the C++ handle owns a fully prepared, immutable float working proxy. It contains no
// decoder or borrowed state, its destructor is thread-independent, and every render allocates
// its edit buffer and libjpeg state locally. C++ contract tests exercise repeated const renders;
// the public Rust wrapper exposes no mutable access to the handle.
unsafe impl Send for ffi::EditPreviewHandle {}
// SAFETY: see the Send implementation above. Concurrent calls only read the working proxy.
unsafe impl Sync for ffi::EditPreviewHandle {}

/// Cache-key version for the fixed-order basic edited-preview recipe.
pub const BASIC_EDIT_PREVIEW_RECIPE_VERSION: u32 = 1;

/// Hard memory bound for the reusable float working proxy.
///
/// A square proxy at this edge consumes at most 192 MiB for interleaved RGB
/// float32. The intended UI values are 1600 and 2048.
pub const MAX_WARM_EDIT_PREVIEW_EDGE: u32 = 4_096;

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
}
