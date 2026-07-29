//! Warm-preview analysis values, execution provenance, and fail-closed wire validation.

use shadow_domain::{ImageDimensions, PreviewCodec};

use super::{BridgeError, decoder::dimensions, ffi};

mod mask_coverage;

pub use mask_coverage::{
    EDIT_PREVIEW_MASK_COVERAGE_SCHEMA_VERSION, EDIT_PREVIEW_MASK_COVERAGE_VERSION,
    EditPreviewMaskCoverage, EditPreviewMaskCoverageRequest, RenderedEditPreview,
};
pub(super) use mask_coverage::{validate_mask_coverage, validate_mask_coverage_request};

/// Number of bins in every warm edit-preview display histogram.
pub const EDIT_PREVIEW_HISTOGRAM_BIN_COUNT: usize = 256;

/// Exact semantic contract for warm edit-preview analysis.
///
/// Histograms cover the complete uncompressed display-encoded sRGB RGB8 warm proxy
/// immediately before JPEG encoding. Clipping counts inspect the edited
/// processed-linear working-RGB samples before display clamping and use strict `< 0` and `> 1`
/// comparisons; exact zero and one are not clipped. HDR headroom bins instead use linear
/// Rec.709 luminance above `1.0` display white and retain the peak positive EV; they are an
/// output-readiness diagnostic, not sensor dynamic-range evidence.
pub const EDIT_PREVIEW_ANALYSIS_VERSION: &str = concat!(
    "shadow.edit-preview-analysis.v2:rgb8-before-jpeg:rec709-encoded-q16:",
    "pre-clamp-linear-strict-lt-gt-any-channel:linear-headroom-log2-v1"
);
pub const EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT: usize = 16;
pub const EDIT_PREVIEW_EXECUTION_RECEIPT_SCHEMA_VERSION: u32 = 1;
pub const EDIT_PREVIEW_EXECUTION_PLAN_CONTRACT_VERSION: u32 = 1;
pub const EDIT_PREVIEW_CPU_ADJUSTMENT_BACKEND_VERSION: u32 = 1;
pub const EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION: u32 = 1;
pub const EDIT_PREVIEW_CPU_DISPLAY_BACKEND_VERSION: u32 = 1;
pub const EDIT_PREVIEW_METAL_DISPLAY_BACKEND_VERSION: u32 = 1;
pub const DISPLAY_SRGB8_OUTPUT_CONTRACT_VERSION: u32 = 1;

/// Per-preview-pixel source headroom information projected from unprocessed RAW samples.
///
/// Bit 0 denotes a CFA sample at its calibrated white level; bit 1 denotes a display cell whose
/// complete source region is at or below calibrated black. Both are source facts, not output
/// histogram thresholds, and no mask is fabricated for JPEG/HEIF or an unavailable provider.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct SensorClippingMask {
    pub available: bool,
    pub dimensions: ImageDimensions,
    pub samples: Vec<u8>,
    pub highlight_pixel_count: u64,
    pub shadow_pixel_count: u64,
}

impl SensorClippingMask {
    pub(crate) const HIGHLIGHT_BIT: u8 = 1 << 0;
    pub(crate) const SHADOW_BIT: u8 = 1 << 1;

    pub(crate) const fn unavailable() -> Self {
        Self {
            available: false,
            dimensions: ImageDimensions {
                width: 0,
                height: 0,
            },
            samples: Vec::new(),
            highlight_pixel_count: 0,
            shadow_pixel_count: 0,
        }
    }
}

/// Transient analysis of one complete warm-proxy edit render.
///
/// The four histograms are derived from uncompressed display-encoded sRGB RGB8 bytes
/// before JPEG encoding. Per-channel and any-channel clipping counts are
/// derived from the same render's processed-linear working-RGB samples before output clamping;
/// they are not sensor-domain exposure measurements.
#[derive(Debug, Clone, PartialEq)]
pub struct EditPreviewAnalysis {
    pub version: String,
    pub sample_dimensions: ImageDimensions,
    pub red: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub green: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub blue: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub luma: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub below_zero_samples: [u64; 3],
    pub above_one_samples: [u64; 3],
    pub hdr_headroom_bins: [u64; EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT],
    pub hdr_headroom_pixels: u64,
    pub hdr_peak_headroom_ev: f64,
    pub pixel_count: u64,
    pub shadow_clipped_pixels: u64,
    pub highlight_clipped_pixels: u64,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash)]
pub enum EditPreviewBackend {
    Cpu,
    Metal,
}

/// Effective adjustment and display route for one completed warm preview.
///
/// `cache_identity` is canonical C++ output and is the only field callers should hash into a
/// durable key. Diagnostic fallback state remains available for inspection but is deliberately
/// absent from that identity.
#[derive(Debug, Clone, Eq, PartialEq, Hash)]
pub struct EditPreviewExecutionReceipt {
    pub schema_version: u32,
    pub cache_identity: String,
    pub adjustment_backend: EditPreviewBackend,
    pub adjustment_backend_version: u32,
    pub adjustment_execution_contract_version: u32,
    pub display_backend: EditPreviewBackend,
    pub display_backend_version: u32,
    pub display_output_contract_version: u32,
    pub fused_pipeline: bool,
    pub adjustment_fell_back: bool,
    pub display_fell_back: bool,
    pub diagnostic: Option<String>,
}

impl EditPreviewExecutionReceipt {
    #[must_use]
    pub const fn uses_current_schema(&self) -> bool {
        self.schema_version == EDIT_PREVIEW_EXECUTION_RECEIPT_SCHEMA_VERSION
    }
}

/// A JPEG preview and its generation-matched transient analysis.
#[derive(Debug, Clone, PartialEq)]
pub struct AnalyzedEditPreview {
    pub proxy: shadow_domain::ProxyPayload,
    pub analysis: EditPreviewAnalysis,
    pub execution: EditPreviewExecutionReceipt,
    /// Exact renderer coverage paired with this preview generation when requested.
    pub mask_coverage: Option<EditPreviewMaskCoverage>,
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

pub(super) fn validate_rgb8_edit_preview(
    proxy: shadow_domain::ProxyPayload,
    expected_dimensions: ImageDimensions,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    let expected_bytes = expected_dimensions
        .pixel_count()
        .checked_mul(3)
        .and_then(|value| usize::try_from(value).ok())
        .ok_or(BridgeError::InvalidEditPreviewOutput(
            "RGB8 preview dimensions exceed the host address space",
        ))?;
    if proxy.dimensions != expected_dimensions
        || proxy.dimensions.width == 0
        || proxy.dimensions.height == 0
        || proxy.codec != PreviewCodec::Bitmap
        || proxy.bits_per_channel != 8
        || proxy.channels != 3
        || proxy.bytes.len() != expected_bytes
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "interactive preview must be tightly packed display-sRGB RGB8",
        ));
    }
    Ok(proxy)
}

pub(super) fn validate_analyzed_edit_preview(
    proxy: shadow_domain::ProxyPayload,
    analysis: ffi::FfiEditPreviewAnalysis,
    execution: ffi::FfiEditPreviewExecutionReceipt,
    expected_dimensions: ImageDimensions,
) -> Result<AnalyzedEditPreview, BridgeError> {
    validate_edit_preview_proxy(&proxy, expected_dimensions)?;
    let analysis = validate_edit_preview_analysis(analysis, expected_dimensions)?;
    let execution = edit_preview_execution_receipt(execution)?;
    Ok(AnalyzedEditPreview {
        proxy,
        analysis,
        execution,
        mask_coverage: None,
    })
}

fn edit_preview_backend(
    backend: ffi::FfiEditPreviewBackend,
) -> Result<EditPreviewBackend, BridgeError> {
    match backend {
        ffi::FfiEditPreviewBackend::Cpu => Ok(EditPreviewBackend::Cpu),
        ffi::FfiEditPreviewBackend::Metal => Ok(EditPreviewBackend::Metal),
        _ => Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt contains an unsupported backend",
        )),
    }
}

pub(super) fn edit_preview_execution_receipt(
    receipt: ffi::FfiEditPreviewExecutionReceipt,
) -> Result<EditPreviewExecutionReceipt, BridgeError> {
    if receipt.schema_version != EDIT_PREVIEW_EXECUTION_RECEIPT_SCHEMA_VERSION {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt uses an unsupported schema",
        ));
    }
    if receipt.cache_identity.is_empty() || receipt.cache_identity.len() > 512 {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt cache identity is empty or unbounded",
        ));
    }
    let adjustment_backend = edit_preview_backend(receipt.adjustment_backend)?;
    let display_backend = edit_preview_backend(receipt.display_backend)?;
    let adjustment_backend_version_is_current = match adjustment_backend {
        EditPreviewBackend::Cpu => {
            receipt.adjustment_backend_version == EDIT_PREVIEW_CPU_ADJUSTMENT_BACKEND_VERSION
        }
        EditPreviewBackend::Metal => {
            receipt.adjustment_backend_version == EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION
        }
    };
    let display_backend_version_is_current = match display_backend {
        EditPreviewBackend::Cpu => {
            receipt.display_backend_version == EDIT_PREVIEW_CPU_DISPLAY_BACKEND_VERSION
        }
        EditPreviewBackend::Metal => {
            receipt.display_backend_version == EDIT_PREVIEW_METAL_DISPLAY_BACKEND_VERSION
        }
    };
    if !adjustment_backend_version_is_current
        || !display_backend_version_is_current
        || (receipt.fused_pipeline && display_backend != EditPreviewBackend::Metal)
        || (receipt.fused_pipeline && (receipt.adjustment_fell_back || receipt.display_fell_back))
        || receipt.adjustment_execution_contract_version
            != EDIT_PREVIEW_EXECUTION_PLAN_CONTRACT_VERSION
        || receipt.display_output_contract_version != DISPLAY_SRGB8_OUTPUT_CONTRACT_VERSION
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt contains unsupported implementation contracts",
        ));
    }
    let any_fallback = receipt.adjustment_fell_back || receipt.display_fell_back;
    if (receipt.adjustment_fell_back && adjustment_backend != EditPreviewBackend::Cpu)
        || (receipt.display_fell_back && display_backend != EditPreviewBackend::Cpu)
        || any_fallback == receipt.diagnostic.is_empty()
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt contains incoherent fallback provenance",
        ));
    }
    Ok(EditPreviewExecutionReceipt {
        schema_version: receipt.schema_version,
        cache_identity: receipt.cache_identity,
        adjustment_backend,
        adjustment_backend_version: receipt.adjustment_backend_version,
        adjustment_execution_contract_version: receipt.adjustment_execution_contract_version,
        display_backend,
        display_backend_version: receipt.display_backend_version,
        display_output_contract_version: receipt.display_output_contract_version,
        fused_pipeline: receipt.fused_pipeline,
        adjustment_fell_back: receipt.adjustment_fell_back,
        display_fell_back: receipt.display_fell_back,
        diagnostic: (!receipt.diagnostic.is_empty()).then_some(receipt.diagnostic),
    })
}

pub(super) fn validate_sensor_clipping_mask(
    mask: ffi::FfiSensorClippingMask,
    expected_dimensions: ImageDimensions,
) -> Result<SensorClippingMask, BridgeError> {
    if !mask.available {
        if mask.dimensions.width != 0
            || mask.dimensions.height != 0
            || !mask.samples.is_empty()
            || mask.highlight_pixel_count != 0
            || mask.shadow_pixel_count != 0
        {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "unavailable sensor clipping mask must not contain data",
            ));
        }
        return Ok(SensorClippingMask::unavailable());
    }

    let dimensions = dimensions(&mask.dimensions);
    if dimensions != expected_dimensions {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "sensor clipping dimensions must match the prepared preview",
        ));
    }
    let expected_len = usize::try_from(dimensions.pixel_count()).map_err(|_| {
        BridgeError::InvalidEditPreviewOutput("sensor clipping mask is too large for this host")
    })?;
    if mask.samples.len() != expected_len {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "sensor clipping mask byte count must equal preview pixel count",
        ));
    }

    let mut highlights = 0_u64;
    let mut shadows = 0_u64;
    for sample in &mask.samples {
        if *sample & !(SensorClippingMask::HIGHLIGHT_BIT | SensorClippingMask::SHADOW_BIT) != 0 {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "sensor clipping mask contains unsupported bits",
            ));
        }
        highlights += u64::from(*sample & SensorClippingMask::HIGHLIGHT_BIT != 0);
        shadows += u64::from(*sample & SensorClippingMask::SHADOW_BIT != 0);
    }
    if highlights != mask.highlight_pixel_count || shadows != mask.shadow_pixel_count {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "sensor clipping counts do not match its mask samples",
        ));
    }
    Ok(SensorClippingMask {
        available: true,
        dimensions,
        samples: mask.samples,
        highlight_pixel_count: highlights,
        shadow_pixel_count: shadows,
    })
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

fn validated_hdr_headroom(
    bins: Vec<u64>,
    headroom_pixels: u64,
    peak_ev: f64,
    pixel_count: u64,
) -> Result<[u64; EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT], BridgeError> {
    let bins: [u64; EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT] = bins.try_into().map_err(|_| {
        BridgeError::InvalidEditPreviewOutput(
            "HDR headroom must contain exactly sixteen one-stop bins",
        )
    })?;
    let sum = bins.iter().try_fold(0_u64, |sum, count| {
        sum.checked_add(*count)
            .ok_or(BridgeError::InvalidEditPreviewOutput(
                "HDR headroom sample count overflows u64",
            ))
    })?;
    if headroom_pixels > pixel_count || sum != headroom_pixels {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "HDR headroom bins must exactly cover their bounded pixel count",
        ));
    }
    if !peak_ev.is_finite()
        || peak_ev < 0.0
        || (headroom_pixels == 0 && peak_ev != 0.0)
        || (headroom_pixels > 0 && peak_ev <= 0.0)
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "HDR headroom peak EV is inconsistent with its sample coverage",
        ));
    }
    Ok(bins)
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

pub(super) fn validate_edit_preview_analysis(
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
    let hdr_headroom_bins = validated_hdr_headroom(
        analysis.hdr_headroom_bins,
        analysis.hdr_headroom_pixels,
        analysis.hdr_peak_headroom_ev,
        analysis.pixel_count,
    )?;

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
        hdr_headroom_bins,
        hdr_headroom_pixels: analysis.hdr_headroom_pixels,
        hdr_peak_headroom_ev: analysis.hdr_peak_headroom_ev,
        pixel_count: analysis.pixel_count,
        shadow_clipped_pixels: analysis.shadow_clipped_pixels,
        highlight_clipped_pixels: analysis.highlight_clipped_pixels,
    })
}
