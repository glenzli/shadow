//! Safe, coarse-grained Rust access to Shadow's C++ image decoder providers.
//!
//! Navigation follows the public contract families in this file: `ffi` defines the generated CXX
//! wire shape; `RawDevelopmentPlan` and receipt types own source-development provenance;
//! `DecodedDisplayLuma` owns bounded proxy analysis; `AdjustmentRenderPlan` owns validated edit
//! execution; preview/detail session types own reusable native buffers; and the final public
//! functions expose inspection and one-shot render entry points. Responsibility-indexed private
//! bridge tests live in `tests`.
//!
//! The generated CXX wire declaration remains centralized and auditable. Safe Rust contract
//! families move into responsibility-named modules when they own independent invariants and
//! failure policy; sharing `ffi` does not require sharing one production file.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
#[cfg(test)]
use shadow_domain::PreviewCodec;
use shadow_domain::{ImageDimensions, RawMetadataSnapshot};
use thiserror::Error;

mod adjustment;
mod decoder;
mod display_luma;
mod optics;
mod preview_analysis;
mod raw_development;

#[cfg(test)]
use adjustment::validate_render_operation;
pub use adjustment::*;
use adjustment::{validate_jpeg_quality, validate_proxy_max_edge, validate_warm_edit_max_edge};
use decoder::{dimensions, open_libraw, open_photo, preview_codec};
pub use decoder::{
    extract_best_libraw_preview, extract_best_photo_preview, inspect_libraw, inspect_photo,
};
pub use display_luma::{
    DecodedDisplayLuma, JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX, MAX_JPEG_DISPLAY_LUMA_EDGE,
    decode_jpeg_display_luma,
};
pub use optics::*;
use optics::{ffi_optics_settings, optics_receipt};
pub use preview_analysis::*;
#[cfg(test)]
use preview_analysis::{edit_preview_execution_receipt, validate_edit_preview_analysis};
use preview_analysis::{validate_analyzed_edit_preview, validate_sensor_clipping_mask};
pub use raw_development::*;
use raw_development::{
    ffi_raw_development_plan, preflight_photo_edit_development, raw_development_capabilities,
    raw_development_plan_negotiation, raw_development_receipt, raw_pipeline_receipt,
};

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
        manual_distortion: i16,
        manual_tca_red_cyan: i16,
        manual_tca_blue_yellow: i16,
        manual_vignetting_amount: i16,
        manual_vignetting_midpoint: u8,
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

    #[derive(Debug)]
    enum FfiRawDevelopmentIntent {
        Preview,
        Detail,
        ExportImage,
    }

    #[derive(Debug)]
    enum FfiRawDevelopmentQuality {
        Fast,
        Balanced,
        High,
    }

    #[derive(Debug)]
    enum FfiDngOpcodePolicy {
        ProviderDefault,
        RequireApplied,
        DeferToShadow,
    }

    #[derive(Debug)]
    enum FfiRawNoiseReductionIntent {
        ProviderDefault,
        Disabled,
        Conservative,
        NoiseRobust,
    }

    #[derive(Debug)]
    enum FfiRawHighlightRecoveryIntent {
        ProviderDefault,
        Disabled,
        Conservative,
        Aggressive,
    }

    #[derive(Debug)]
    enum FfiRawDevelopmentPlanNegotiationStatus {
        Accepted,
        Adjusted,
        Rejected,
    }

    #[derive(Debug)]
    enum FfiDngOpcodeExecutionStatus {
        NotDeclared,
        ProviderDefault,
        Applied,
        DeferredToShadow,
        SkippedForPreview,
        Unsupported,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiRawDevelopmentPlan {
        schema_version: u32,
        intent: FfiRawDevelopmentIntent,
        quality: FfiRawDevelopmentQuality,
        dng_opcode_policy: FfiDngOpcodePolicy,
        noise_reduction: FfiRawNoiseReductionIntent,
        highlight_recovery: FfiRawHighlightRecoveryIntent,
    }

    #[derive(Debug, Clone, Copy)]
    #[allow(clippy::struct_excessive_bools)]
    struct FfiRawDevelopmentCapabilities {
        schema_version: u32,
        available: bool,
        raw_frame: bool,
        dng_opcode_execution_receipt: bool,
        supported_intents: u32,
        supported_qualities: u32,
        supported_dng_opcode_policies: u32,
        supported_noise_reduction_intents: u32,
        supported_highlight_recovery_intents: u32,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiRawDevelopmentPlanNegotiation {
        requested: FfiRawDevelopmentPlan,
        effective: FfiRawDevelopmentPlan,
        status: FfiRawDevelopmentPlanNegotiationStatus,
        unresolved: u32,
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
        requested_plan_identity: String,
        effective_plan_identity: String,
        requested_plan: FfiRawDevelopmentPlan,
        effective_plan: FfiRawDevelopmentPlan,
        plan_negotiation_status: FfiRawDevelopmentPlanNegotiationStatus,
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
        dng_opcode_list_1_execution: FfiDngOpcodeExecutionStatus,
        dng_opcode_list_2_execution: FfiDngOpcodeExecutionStatus,
        dng_opcode_list_3_execution: FfiDngOpcodeExecutionStatus,
        process_warnings: u32,
    }

    #[derive(Debug)]
    enum FfiRawPipelinePath {
        DecodedRaster,
        ShadowRawFrame,
        ProviderProcessedCompatibility,
    }

    #[derive(Debug)]
    enum FfiRawCameraProfileStatus {
        NotConsidered,
        NoMatch,
        Applied,
        MatchedNotApplied,
    }

    // Host-side route provenance. `cache_identity` is the canonical composite identity produced
    // by C++; `pipeline_identity` identifies only the selected algorithm/compatibility stage.
    #[derive(Debug)]
    struct FfiRawPipelineReceipt {
        schema_version: u32,
        path: FfiRawPipelinePath,
        cache_identity: String,
        pipeline_identity: String,
        source_provider_id: String,
        source_provider_version: String,
        fallback_reason: String,
        raw_frame_schema_version: u32,
        raw_developer_version: u32,
        requested_plan: FfiRawDevelopmentPlan,
        effective_plan: FfiRawDevelopmentPlan,
        camera_profile_status: FfiRawCameraProfileStatus,
        camera_profile_catalog_identity: String,
        camera_profile_identity: String,
        camera_profile_name: String,
        camera_profile_diagnostic: String,
        camera_profile_developer_version: u32,
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
        raw_frame: bool,
        reference_rgb: bool,
        dng_opcode_list_1_bytes: u32,
        dng_opcode_list_2_bytes: u32,
        dng_opcode_list_3_bytes: u32,
        raw_development: FfiRawDevelopmentCapabilities,
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
        hdr_headroom_bins: Vec<u64>,
        hdr_headroom_pixels: u64,
        hdr_peak_headroom_ev: f64,
        pixel_count: u64,
        shadow_clipped_pixels: u64,
        highlight_clipped_pixels: u64,
    }

    #[derive(Debug)]
    enum FfiEditPreviewBackend {
        Cpu,
        Metal,
    }

    #[derive(Debug)]
    struct FfiEditPreviewExecutionReceipt {
        schema_version: u32,
        cache_identity: String,
        adjustment_backend: FfiEditPreviewBackend,
        adjustment_backend_version: u32,
        adjustment_execution_contract_version: u32,
        display_backend: FfiEditPreviewBackend,
        display_backend_version: u32,
        display_output_contract_version: u32,
        fused_pipeline: bool,
        adjustment_fell_back: bool,
        display_fell_back: bool,
        diagnostic: String,
    }

    #[derive(Debug)]
    struct FfiAnalyzedEditPreview {
        proxy: FfiEncodedProxy,
        analysis: FfiEditPreviewAnalysis,
        execution: FfiEditPreviewExecutionReceipt,
    }

    #[derive(Debug)]
    struct FfiCancellableEncodedProxy {
        cancelled: bool,
        proxy: FfiEncodedProxy,
    }

    #[derive(Debug)]
    struct FfiCancellableAnalyzedEditPreview {
        cancelled: bool,
        preview: FfiAnalyzedEditPreview,
    }

    /// A compact RAW-source diagnostic in the exact display dimensions of the prepared preview.
    /// It is absent for display-referred sources and for providers that cannot supply RawFrame.
    #[derive(Debug)]
    struct FfiSensorClippingMask {
        available: bool,
        dimensions: FfiDimensions,
        samples: Vec<u8>,
        highlight_pixel_count: u64,
        shadow_pixel_count: u64,
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
        LocalMaskLayerStart,
        LocalMaskLayerEnd,
        Exposure,
        Contrast,
        OklabLightnessToneCurve,
        RgbWhiteBalance,
        Saturation,
        SelectiveTone,
        PerceptualColor,
        OklabColorWarper,
        Lut3D,
        Sharpen,
        SpotHeal,
    }

    #[derive(Debug, Clone, Copy)]
    enum FfiDetailEffectsPass {
        TechnicalDetail,
        ColorGrading,
        FinishingEffects,
    }

    #[derive(Debug)]
    struct FfiAdjustmentNode {
        node_id: String,
        operation: FfiAdjustmentOperation,
        detail_effects_pass: FfiDetailEffectsPass,
        parameter_schema_version: u32,
        implementation_version: u32,
        enabled: bool,
        parameters: Vec<f64>,
        /// Operation-specific immutable binary document. Only Lut3D accepts
        /// a validated `.cube` document; every other operation requires empty.
        payload: Vec<u8>,
        /// Lengths for grouped variable-size parameters. Perceptual Color
        /// stores the number of additional sampled color ranges; Spot Heal
        /// stores its target count.
        parameter_group_lengths: Vec<u32>,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiPhotoGeometry {
        crop_left: f64,
        crop_top: f64,
        crop_right: f64,
        crop_bottom: f64,
        quarter_turn: u8,
        straighten_degrees: f64,
        flip_horizontal: bool,
        flip_vertical: bool,
    }

    #[derive(Debug)]
    struct FfiAdjustmentRenderRequest {
        nodes: Vec<FfiAdjustmentNode>,
        geometry: FfiPhotoGeometry,
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
        geometry: FfiPhotoGeometry,
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
        type EditPreviewCancellationHandle;
        type FullEditDetailHandle;

        fn open_libraw_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn open_photo_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn query_libraw_optics_profiles_utf8(path: &str) -> Result<Vec<FfiOpticsProfileCandidate>>;
        fn query_photo_optics_profiles_utf8(path: &str) -> Result<Vec<FfiOpticsProfileCandidate>>;
        fn query_optics_profiles_for_metadata(
            metadata: &FfiMetadataSnapshot,
        ) -> Vec<FfiOpticsProfileCandidate>;
        fn libraw_provider_version() -> String;
        fn photo_provider_version() -> String;
        fn edit_preview_generator_implementation_identity() -> String;
        fn new_edit_preview_cancellation() -> Result<SharedPtr<EditPreviewCancellationHandle>>;
        fn cancel(self: &EditPreviewCancellationHandle) -> bool;
        fn photo_supported_raster_extensions() -> Vec<String>;
        fn raw_development_plan_identity(plan: &FfiRawDevelopmentPlan) -> Result<String>;
        fn render_photo_reference_proxy(
            path: &str,
            max_edge: u32,
            jpeg_quality: u8,
        ) -> Result<FfiEncodedProxy>;
        fn decode_jpeg_display_luma(encoded: &[u8], max_edge: u32) -> Result<FfiDisplayLuma>;
        fn provider(self: &DecodeHandle) -> FfiProviderSnapshot;
        fn metadata(self: &DecodeHandle) -> FfiMetadataSnapshot;
        fn capabilities(self: &DecodeHandle) -> FfiCapabilitySnapshot;
        fn raw_development_capabilities(self: &DecodeHandle) -> FfiRawDevelopmentCapabilities;
        fn negotiate_raw_development_plan(
            self: &DecodeHandle,
            plan: &FfiRawDevelopmentPlan,
        ) -> Result<FfiRawDevelopmentPlanNegotiation>;
        // The current public Rust API opens a prepared session directly. Keep this lower-level
        // read-only accessor for C++/future bridge callers, where it reports an explicit default
        // before a source render is prepared.
        #[allow(dead_code)]
        fn raw_development_receipt(self: &DecodeHandle) -> Result<FfiRawDevelopmentReceipt>;
        #[allow(dead_code)]
        fn raw_pipeline_receipt(self: &DecodeHandle) -> Result<FfiRawPipelineReceipt>;
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
        #[allow(dead_code)]
        fn prepare_edit_preview(
            self: &DecodeHandle,
            max_edge: u32,
        ) -> Result<UniquePtr<EditPreviewHandle>>;
        fn prepare_edit_preview_with_raw_development_plan(
            self: &DecodeHandle,
            max_edge: u32,
            plan: &FfiRawDevelopmentPlan,
        ) -> Result<UniquePtr<EditPreviewHandle>>;
        #[allow(dead_code)]
        fn prepare_edit_detail(self: &DecodeHandle) -> Result<UniquePtr<FullEditDetailHandle>>;
        fn prepare_edit_detail_with_raw_development_plan(
            self: &DecodeHandle,
            plan: &FfiRawDevelopmentPlan,
        ) -> Result<UniquePtr<FullEditDetailHandle>>;
        fn dimensions(self: &EditPreviewHandle) -> FfiDimensions;
        fn max_edge(self: &EditPreviewHandle) -> u32;
        fn optics_receipt(self: &EditPreviewHandle) -> FfiOpticsReceipt;
        fn raw_development_receipt(self: &EditPreviewHandle) -> Result<FfiRawDevelopmentReceipt>;
        fn raw_pipeline_receipt(self: &EditPreviewHandle) -> Result<FfiRawPipelineReceipt>;
        fn sensor_clipping_mask(self: &EditPreviewHandle) -> FfiSensorClippingMask;
        fn render_adjustment_plan(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiEncodedProxy>;
        fn render_adjustment_plan_with_analysis(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiAnalyzedEditPreview>;
        fn render_adjustment_plan_cancellable(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
            cancellation: &EditPreviewCancellationHandle,
        ) -> Result<FfiCancellableEncodedProxy>;
        fn render_adjustment_plan_with_analysis_cancellable(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
            cancellation: &EditPreviewCancellationHandle,
        ) -> Result<FfiCancellableAnalyzedEditPreview>;
        fn dimensions(self: &FullEditDetailHandle) -> FfiDimensions;
        fn retained_bytes(self: &FullEditDetailHandle) -> u64;
        fn optics_receipt(self: &FullEditDetailHandle) -> FfiOpticsReceipt;
        fn raw_development_receipt(self: &FullEditDetailHandle)
        -> Result<FfiRawDevelopmentReceipt>;
        fn raw_pipeline_receipt(self: &FullEditDetailHandle) -> Result<FfiRawPipelineReceipt>;
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

// SAFETY: the native handle owns only std::stop_source. request_stop() and token copies are
// thread-safe by the C++20 stop-token contract, and Rust receives it only through SharedPtr.
unsafe impl Send for ffi::EditPreviewCancellationHandle {}
// SAFETY: see Send above; all shared access is const except stop_source's synchronized
// request_stop operation.
unsafe impl Sync for ffi::EditPreviewCancellationHandle {}

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

/// Hard width and height bound for one full-resolution detail tile.
pub const MAX_EDIT_DETAIL_TILE_SIDE: u32 = 1_024;

/// Hard bound for the complete immutable u16 source retained by one detail session.
pub const MAX_EDIT_DETAIL_RETAINED_BYTES: u64 = 512 * 1_024 * 1_024;

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

/// Returns the cache-safe implementation contract for warm edit-preview generation.
///
/// This identifies compiled adjustment, display and JPEG contracts, not the effective backend
/// of one render. [`AnalyzedEditPreview::execution`] records that post-selection CPU/Metal route.
#[must_use]
pub fn edit_preview_generator_implementation_identity() -> String {
    ffi::edit_preview_generator_implementation_identity()
}

/// Returns ordinary rendered-image suffixes that the linked native photo router can genuinely
/// open. JPEG is mandatory; HEIF/HEIC is added only when this particular build linked libheif.
#[must_use]
pub fn photo_supported_raster_extensions() -> Vec<String> {
    ffi::photo_supported_raster_extensions()
}

/// Returns the native, canonical cache identity for a source-development request. Keeping this
/// calculation in the C++ image contract prevents Rust, desktop, and a future private provider
/// from accidentally serializing equivalent plans differently.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidRawDevelopmentPlan`] when the plan schema is unsupported, or
/// a decoder bridge error if the native contract rejects an invalid enum representation.
pub fn raw_development_plan_identity(plan: RawDevelopmentPlan) -> Result<String, BridgeError> {
    plan.validate()?;
    Ok(ffi::raw_development_plan_identity(
        &ffi_raw_development_plan(plan),
    )?)
}

/// Opens the source-neutral provider just long enough to negotiate a RAW plan. It does not
/// decode pixels or prepare an edit session. Raster sources correctly report a rejected
/// negotiation because a RAW plan has no effect on their already-rendered source pixels.
///
/// # Errors
///
/// Returns an invalid-plan, path, or decoder bridge error.
pub fn negotiate_photo_raw_development_plan(
    path: &Path,
    plan: RawDevelopmentPlan,
) -> Result<RawDevelopmentPlanNegotiation, BridgeError> {
    plan.validate()?;
    let handle = open_photo(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    raw_development_plan_negotiation(
        handle.negotiate_raw_development_plan(&ffi_raw_development_plan(plan))?,
    )
}

/// Reads the provider-neutral RAW-development capability declaration without rendering pixels.
/// This is the preflight counterpart to [`negotiate_photo_raw_development_plan`]: callers can
/// present only meaningful plan choices before they request a preview or a 1:1 detail source.
///
/// # Errors
///
/// Returns a path or decoder bridge error.
pub fn photo_raw_development_capabilities(
    path: &Path,
) -> Result<RawDevelopmentCapabilities, BridgeError> {
    let handle = open_photo(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    Ok(raw_development_capabilities(
        handle.raw_development_capabilities(),
    ))
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
    sensor_clipping_mask: SensorClippingMask,
    raw_development_receipt: RawDevelopmentReceipt,
    raw_pipeline_receipt: RawPipelineReceipt,
    optics_receipt: OpticsReceipt,
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
    raw_pipeline_receipt: RawPipelineReceipt,
    optics_receipt: OpticsReceipt,
}

/// Source-neutral name for an immutable interactive photo-editing session.
///
/// The legacy `LibRawEditPreviewSession` name remains available for source compatibility, while
/// the constructor now enters through Shadow's photo router. RAW receipts remain explicit and
/// absent for a raster source that did not perform RAW development.
pub type PhotoEditPreviewSession = LibRawEditPreviewSession;

/// One-shot cancellation shared by all clones of this handle.
///
/// Cancelling is idempotent: the first call returns `true`, while later calls return `false`.
/// A cancelled handle stays cancelled and is intentionally not reusable for a later render.
#[derive(Clone)]
pub struct EditPreviewCancellation {
    handle: cxx::SharedPtr<ffi::EditPreviewCancellationHandle>,
}

impl std::fmt::Debug for EditPreviewCancellation {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("EditPreviewCancellation")
            .finish_non_exhaustive()
    }
}

impl EditPreviewCancellation {
    /// Creates a new independent cancellation source.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::NullHandle`] if the native bridge cannot allocate its shared
    /// cancellation source.
    pub fn new() -> Result<Self, BridgeError> {
        let handle = ffi::new_edit_preview_cancellation()?;
        if handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        Ok(Self { handle })
    }

    /// Requests cancellation. Returns `true` only for the first successful request.
    pub fn cancel(&self) -> bool {
        self.handle
            .as_ref()
            .is_some_and(ffi::EditPreviewCancellationHandle::cancel)
    }
}

/// Terminal native preview outcome. Cancellation is control flow, never a decoder/backend error.
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum CancellableEditPreview<T> {
    Completed(T),
    Cancelled,
}

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
            .field("raw_pipeline_receipt", &self.raw_pipeline_receipt)
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
            .field(
                "sensor_clipping_available",
                &self.sensor_clipping_mask.available,
            )
            .field("raw_development_receipt", &self.raw_development_receipt)
            .field("raw_pipeline_receipt", &self.raw_pipeline_receipt)
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
        Self::open_with_raw_development_plan_and_optics(
            path,
            max_edge,
            RawDevelopmentPlan::preview(),
            optics,
        )
    }

    /// Opens a preview with an explicit RAW source-development request. The plan is validated
    /// before the source path is opened; `Preview` intent is required because the prepared
    /// session is a bounded interactive raster rather than a native-detail source.
    pub fn open_with_raw_development_plan(
        path: &Path,
        max_edge: u32,
        raw_development_plan: RawDevelopmentPlan,
    ) -> Result<Self, BridgeError> {
        Self::open_with_raw_development_plan_and_optics(
            path,
            max_edge,
            raw_development_plan,
            &OpticsSettings::default(),
        )
    }

    /// Opens a preview with explicit RAW source-development and optical-correction contracts.
    /// JPEG/HEIF sources retain their ordinary decoded-raster behavior; they never fabricate a
    /// RAW receipt merely because a caller supplied the canonical preview plan.
    pub fn open_with_raw_development_plan_and_optics(
        path: &Path,
        max_edge: u32,
        raw_development_plan: RawDevelopmentPlan,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        validate_warm_edit_max_edge(max_edge)?;
        raw_development_plan.validate()?;
        if raw_development_plan.intent != RawDevelopmentIntent::Preview {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "warm edit previews require preview RAW-development intent",
            ));
        }
        let mut decode_handle = open_photo(path)?;
        if decode_handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        {
            let handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
            preflight_photo_edit_development(handle, raw_development_plan)?;
        }
        decode_handle
            .pin_mut()
            .configure_optics(&ffi_optics_settings(optics))?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_preview_with_raw_development_plan(
            max_edge,
            &ffi_raw_development_plan(raw_development_plan),
        )?;
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let prepared_max_edge = prepared.max_edge();
        // This is optional inspection data prepared alongside the immutable source raster. It
        // must never reopen or unpack a RAW file merely to drive a zebra overlay.
        let sensor_clipping_mask = match validate_sensor_clipping_mask(
            prepared.sensor_clipping_mask(),
            prepared_dimensions,
        ) {
            Ok(mask) => mask,
            Err(error) => {
                eprintln!("Shadow: ignoring invalid RAW clipping diagnostic: {error}");
                SensorClippingMask::unavailable()
            }
        };
        let raw_development_receipt = raw_development_receipt(prepared.raw_development_receipt()?)?;
        let raw_pipeline_receipt = raw_pipeline_receipt(prepared.raw_pipeline_receipt()?)?;
        let optics_receipt = optics_receipt(prepared.optics_receipt());

        Ok(Self {
            handle,
            dimensions: prepared_dimensions,
            max_edge: prepared_max_edge,
            sensor_clipping_mask,
            raw_development_receipt,
            raw_pipeline_receipt,
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

    /// Returns source-domain clipping information computed once while this immutable preview was
    /// prepared. Slider renders reuse this data and do not reopen or unpack the RAW file.
    #[must_use]
    pub const fn sensor_clipping_mask(&self) -> &SensorClippingMask {
        &self.sensor_clipping_mask
    }

    /// Returns immutable provenance for the exact RAW development, if any, retained by this
    /// preview.
    #[must_use]
    pub const fn raw_development_receipt(&self) -> &RawDevelopmentReceipt {
        &self.raw_development_receipt
    }

    /// Returns the typed host-side route and canonical cache identity that produced this preview.
    #[must_use]
    pub const fn raw_pipeline_receipt(&self) -> &RawPipelineReceipt {
        &self.raw_pipeline_receipt
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
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let proxy = handle.render_adjustment_plan(&request)?;
        let proxy = proxy_payload(proxy);
        if proxy.dimensions != output_dimensions {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "geometry-aware preview dimensions do not match the rendered canvas",
            ));
        }
        Ok(proxy)
    }

    /// Executes a typed plan with cooperative native cancellation.
    ///
    /// A cancelled render returns [`CancellableEditPreview::Cancelled`] and never fabricates a
    /// backend failure, fallback receipt, histogram, or JPEG. The cancellation handle is
    /// one-shot; create a fresh handle for each independently cancellable render.
    pub fn render_plan_cancellable(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CancellableEditPreview<shadow_domain::ProxyPayload>, BridgeError> {
        plan.validate()?;
        validate_jpeg_quality(jpeg_quality)?;
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let cancellation = cancellation
            .handle
            .as_ref()
            .ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let rendered = handle.render_adjustment_plan_cancellable(&request, cancellation)?;
        if rendered.cancelled {
            return Ok(CancellableEditPreview::Cancelled);
        }
        let proxy = proxy_payload(rendered.proxy);
        if proxy.dimensions != output_dimensions {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "geometry-aware preview dimensions do not match the rendered canvas",
            ));
        }
        Ok(CancellableEditPreview::Completed(proxy))
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
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let analyzed = handle.render_adjustment_plan_with_analysis(&request)?;
        let proxy = proxy_payload(analyzed.proxy);
        validate_analyzed_edit_preview(
            proxy,
            analyzed.analysis,
            analyzed.execution,
            output_dimensions,
        )
    }

    /// Executes a typed plan with generation-matched analysis and cooperative cancellation.
    ///
    /// Cancellation before completion returns no partial pixels, analysis, or execution receipt.
    pub fn render_plan_with_analysis_cancellable(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CancellableEditPreview<AnalyzedEditPreview>, BridgeError> {
        plan.validate()?;
        validate_jpeg_quality(jpeg_quality)?;
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let cancellation = cancellation
            .handle
            .as_ref()
            .ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let rendered =
            handle.render_adjustment_plan_with_analysis_cancellable(&request, cancellation)?;
        if rendered.cancelled {
            return Ok(CancellableEditPreview::Cancelled);
        }
        let analyzed = rendered.preview;
        let proxy = proxy_payload(analyzed.proxy);
        let completed = validate_analyzed_edit_preview(
            proxy,
            analyzed.analysis,
            analyzed.execution,
            output_dimensions,
        )?;
        Ok(CancellableEditPreview::Completed(completed))
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
        Self::open_with_raw_development_plan_and_optics(path, RawDevelopmentPlan::detail(), optics)
    }

    /// Opens an immutable full-resolution session with an explicit RAW source-development plan.
    /// Detail and ExportImage intents are accepted; Preview is rejected so a warm half-size
    /// source can never enter a full-resolution pipeline.
    pub fn open_with_raw_development_plan(
        path: &Path,
        raw_development_plan: RawDevelopmentPlan,
    ) -> Result<Self, BridgeError> {
        Self::open_with_raw_development_plan_and_optics(
            path,
            raw_development_plan,
            &OpticsSettings::default(),
        )
    }

    /// Opens a native-detail session with explicit RAW source-development and optical settings.
    pub fn open_with_raw_development_plan_and_optics(
        path: &Path,
        raw_development_plan: RawDevelopmentPlan,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        raw_development_plan.validate()?;
        if !matches!(
            raw_development_plan.intent,
            RawDevelopmentIntent::Detail | RawDevelopmentIntent::ExportImage
        ) {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "full-resolution edit requires detail or export-image RAW-development intent",
            ));
        }
        let mut decode_handle = open_photo(path)?;
        if decode_handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        {
            let handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
            preflight_photo_edit_development(handle, raw_development_plan)?;
        }
        decode_handle
            .pin_mut()
            .configure_optics(&ffi_optics_settings(optics))?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_detail_with_raw_development_plan(
            &ffi_raw_development_plan(raw_development_plan),
        )?;
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let retained_bytes = prepared.retained_bytes();
        let raw_development_receipt = raw_development_receipt(prepared.raw_development_receipt()?)?;
        let raw_pipeline_receipt = raw_pipeline_receipt(prepared.raw_pipeline_receipt()?)?;
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
            raw_pipeline_receipt,
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

    /// Returns the typed host-side route and canonical cache identity for this retained source.
    #[must_use]
    pub const fn raw_pipeline_receipt(&self) -> &RawPipelineReceipt {
        &self.raw_pipeline_receipt
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
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        request.validate(output_dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let rendered =
            handle.render_adjustment_plan_tile(&ffi_detail_tile_request(plan, request))?;
        let rect = detail_tile_rect(rendered.rect);
        let full_dimensions = dimensions(&rendered.full_dimensions);
        if rect != request.rect || full_dimensions != output_dimensions {
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

fn ffi_render_request(
    plan: &AdjustmentRenderPlan,
    max_edge: u32,
    jpeg_quality: u8,
) -> ffi::FfiAdjustmentRenderRequest {
    ffi::FfiAdjustmentRenderRequest {
        nodes: plan.nodes.iter().map(ffi_render_node).collect(),
        geometry: ffi_photo_geometry(plan.geometry),
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
        geometry: ffi_photo_geometry(plan.geometry),
        rect: ffi_detail_tile_rect(request.rect),
    }
}

const fn ffi_photo_geometry(geometry: AdjustmentGeometry) -> ffi::FfiPhotoGeometry {
    ffi::FfiPhotoGeometry {
        crop_left: geometry.crop_left,
        crop_top: geometry.crop_top,
        crop_right: geometry.crop_right,
        crop_bottom: geometry.crop_bottom,
        quarter_turn: match geometry.quarter_turn {
            AdjustmentQuarterTurn::Zero => 0,
            AdjustmentQuarterTurn::Clockwise90 => 1,
            AdjustmentQuarterTurn::Clockwise180 => 2,
            AdjustmentQuarterTurn::Clockwise270 => 3,
        },
        straighten_degrees: geometry.straighten_degrees,
        flip_horizontal: geometry.flip_horizontal,
        flip_vertical: geometry.flip_vertical,
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
        AdjustmentRenderOperation::LocalMaskLayerStart { opacity, mask } => {
            let (kind, x0, y0, x1, y1, radius_x, radius_y, feather, invert, brush_points) =
                match mask {
                    None => (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, Vec::new()),
                    Some(AdjustmentLocalMask::LinearGradient {
                        start_x,
                        start_y,
                        end_x,
                        end_y,
                        invert,
                    }) => (
                        1.0,
                        *start_x,
                        *start_y,
                        *end_x,
                        *end_y,
                        0.0,
                        0.0,
                        0.0,
                        if *invert { 1.0 } else { 0.0 },
                        Vec::new(),
                    ),
                    Some(AdjustmentLocalMask::RadialGradient {
                        center_x,
                        center_y,
                        radius_x,
                        radius_y,
                        feather,
                        invert,
                    }) => (
                        2.0,
                        *center_x,
                        *center_y,
                        0.0,
                        0.0,
                        *radius_x,
                        *radius_y,
                        *feather,
                        if *invert { 1.0 } else { 0.0 },
                        Vec::new(),
                    ),
                    Some(AdjustmentLocalMask::Brush {
                        points,
                        radius,
                        feather,
                        invert,
                    }) => (
                        3.0,
                        0.0,
                        0.0,
                        0.0,
                        0.0,
                        *radius,
                        0.0,
                        *feather,
                        if *invert { 1.0 } else { 0.0 },
                        points
                            .iter()
                            .flat_map(|point| {
                                [
                                    point.x,
                                    point.y,
                                    if point.begins_stroke { 1.0 } else { 0.0 },
                                ]
                            })
                            .collect(),
                    ),
                };
            let point_count = u32::try_from(brush_points.len() / 3)
                .expect("validated brush point count fits in u32");
            let mut parameters = vec![
                *opacity, kind, x0, y0, x1, y1, radius_x, radius_y, feather, invert,
            ];
            parameters.extend(brush_points);
            (
                ffi::FfiAdjustmentOperation::LocalMaskLayerStart,
                parameters,
                if kind == 3.0 {
                    vec![point_count]
                } else {
                    vec![]
                },
                vec![],
            )
        }
        AdjustmentRenderOperation::LocalMaskLayerEnd => (
            ffi::FfiAdjustmentOperation::LocalMaskLayerEnd,
            vec![],
            vec![],
            vec![],
        ),
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
        AdjustmentRenderOperation::OklabLightnessToneCurve { curve } => (
            ffi::FfiAdjustmentOperation::OklabLightnessToneCurve,
            curve
                .lightness
                .iter()
                .flat_map(|point| [point.x, point.y])
                .collect(),
            vec![],
            vec![],
        ),
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
                Vec::with_capacity(72 + parameters.additional_color_ranges.len() * 7);
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
            flattened.push(if parameters.selective_color_relative {
                1.0
            } else {
                0.0
            });
            flattened.push(parameters.selective_color_lightness_protection);
            flattened.extend(parameters.selective_color_cmyk);
            // Preserve the original v1 Color Mixer / Point Color order and
            // add global opponent controls before the variable range tail.
            flattened.extend([parameters.global_a_balance, parameters.global_b_balance]);
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
        AdjustmentRenderOperation::OklabColorWarper { parameters } => {
            // The wire order is strength followed by row-major `(a, b)` pairs.
            // It is deliberately fixed-size: a Recipe and the native lattice
            // can never disagree about topology.
            let mut flattened = Vec::with_capacity(1 + OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT * 2);
            flattened.push(parameters.strength);
            for point in &parameters.control_points {
                flattened.extend([point.a_offset, point.b_offset]);
            }
            (
                ffi::FfiAdjustmentOperation::OklabColorWarper,
                flattened,
                vec![],
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
        AdjustmentRenderOperation::Sharpen { parameters, .. } => {
            let mut flattened = vec![
                parameters.amount,
                parameters.radius,
                parameters.threshold,
                parameters.masking,
            ];
            flattened.extend([
                parameters.clarity,
                parameters.texture,
                parameters.local_contrast,
                parameters.local_contrast_scale,
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
        AdjustmentRenderOperation::SpotHeal { targets, strokes } => {
            let stroke_parameters = strokes
                .iter()
                .map(|stroke| 5 + stroke.points.len() * 2)
                .sum::<usize>();
            let mut flattened = Vec::with_capacity(targets.len() * 7 + stroke_parameters);
            let mut parameter_group_lengths = vec![
                u32::try_from(targets.len()).expect("validated spot-heal target count fits u32"),
                u32::try_from(strokes.len()).expect("validated continuous stroke count fits u32"),
            ];
            for target in targets {
                flattened.extend([
                    target.center_x,
                    target.center_y,
                    f64::from(target.radius_level_zero_pixels),
                    f64::from(target.mode),
                    target.source_offset_x_radii,
                    target.source_offset_y_radii,
                    target.feather,
                ]);
            }
            for stroke in strokes {
                parameter_group_lengths.push(
                    u32::try_from(stroke.points.len())
                        .expect("validated continuous stroke point count fits u32"),
                );
                flattened.extend([
                    f64::from(stroke.radius_level_zero_pixels),
                    f64::from(stroke.mode),
                    stroke.source_offset_x_radii,
                    stroke.source_offset_y_radii,
                    stroke.feather,
                ]);
                for point in &stroke.points {
                    flattened.extend([point.x, point.y]);
                }
            }
            (
                ffi::FfiAdjustmentOperation::SpotHeal,
                flattened,
                parameter_group_lengths,
                vec![],
            )
        }
    };
    let detail_effects_pass = match &node.operation {
        AdjustmentRenderOperation::Sharpen {
            pass: AdjustmentDetailEffectsPass::TechnicalDetail,
            ..
        } => ffi::FfiDetailEffectsPass::TechnicalDetail,
        AdjustmentRenderOperation::Sharpen {
            pass: AdjustmentDetailEffectsPass::ColorGrading,
            ..
        } => ffi::FfiDetailEffectsPass::ColorGrading,
        AdjustmentRenderOperation::Sharpen {
            pass: AdjustmentDetailEffectsPass::FinishingEffects,
            ..
        } => ffi::FfiDetailEffectsPass::FinishingEffects,
        _ => ffi::FfiDetailEffectsPass::TechnicalDetail,
    };
    ffi::FfiAdjustmentNode {
        node_id: node.node_id.clone(),
        operation,
        detail_effects_pass,
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

#[derive(Debug, Error)]
pub enum BridgeError {
    #[error("invalid RAW development plan: {0}")]
    InvalidRawDevelopmentPlan(&'static str),
    #[error("invalid RAW pipeline receipt: {0}")]
    InvalidRawPipelineReceipt(&'static str),
    #[error("invalid edited proxy request: {0}")]
    InvalidEditRequest(&'static str),
    #[error("invalid edit-preview analysis bridge output: {0}")]
    InvalidEditPreviewOutput(&'static str),
    #[error("invalid full edit detail bridge output: {0}")]
    InvalidEditDetailOutput(&'static str),
    #[error("RAW development is unavailable: {0}")]
    RawDevelopmentUnavailable(&'static str),
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

#[cfg(test)]
mod tests;
