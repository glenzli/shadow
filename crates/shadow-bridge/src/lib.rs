//! Safe, coarse-grained Rust access to Shadow's C++ image decoder providers.
//!
//! Navigation starts with the responsibility-named modules below: decoder and provider entry
//! points; RAW, optics, adjustment, and preview-analysis contracts; preview and detail session
//! lifecycles; stateless rendering; flat wire mapping; and the shared error vocabulary.
//! Responsibility-indexed private bridge tests live in `tests`.
//!
//! This file deliberately retains the generated CXX wire declaration as one auditable ABI
//! boundary and re-exports the safe public contract. Sharing `ffi` does not require unrelated
//! Rust behavior to share this source file.

mod adjustment;
mod decoder;
mod detail_session;
mod display_luma;
mod error;
mod one_shot;
mod optics;
mod preview_analysis;
mod preview_frame;
mod preview_session;
mod provider;
mod raw_development;
mod raw_foundation;
mod render_wire;

pub use adjustment::*;
pub use decoder::{
    extract_best_libraw_preview, extract_best_photo_preview, inspect_libraw, inspect_photo,
};
pub use detail_session::*;
pub use display_luma::{
    DecodedDisplayLuma, JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX, MAX_JPEG_DISPLAY_LUMA_EDGE,
    decode_jpeg_display_luma,
};
pub use error::BridgeError;
pub use one_shot::*;
pub use optics::*;
pub use preview_analysis::*;
pub use preview_frame::*;
pub use preview_session::*;
pub use provider::*;
pub use raw_development::*;
pub use raw_foundation::*;

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
    enum FfiRawWhiteBalanceMode {
        AsShot,
        TemperatureTint,
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
        white_balance_mode: FfiRawWhiteBalanceMode,
        temperature_kelvin: u32,
        tint: i16,
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

    // Rust owns this complete buffer for one synchronous C++ preparation call. Native code
    // borrows the float slice and copies only into the resulting scene-linear session; no path
    // or artifact file descriptor crosses the ABI.
    #[derive(Debug)]
    struct FfiRawFoundation {
        width: u32,
        height: u32,
        crop_top: u32,
        crop_left: u32,
        amount_percent: u8,
        source_sha256: String,
        artifact_file_sha256: String,
        cache_key_sha256: String,
        model_identity: String,
        implementation_revision: String,
        samples: Vec<f32>,
    }

    #[derive(Debug)]
    struct FfiOpticsProfileCandidate {
        camera_maker: String,
        camera_model: String,
        lens_maker: String,
        lens_model: String,
    }

    #[derive(Debug)]
    struct FfiRawWhiteBalancePresentation {
        available: bool,
        temperature_kelvin: u32,
        tint: i16,
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
        has_gps_coordinates: bool,
        gps_latitude_degrees: f64,
        gps_longitude_degrees: f64,
        has_gps_altitude: bool,
        gps_altitude_meters: f64,
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

    /// Optional exact local-mask coverage paired with one completed preview.
    ///
    /// `available == false` requires every remaining field to use its empty
    /// sentinel. Selection revision is host transaction metadata and therefore
    /// never crosses this native boundary.
    #[derive(Debug)]
    struct FfiEditPreviewMaskCoverage {
        available: bool,
        version: String,
        layer_index: u32,
        dimensions: FfiDimensions,
        row_stride_bytes: u32,
        samples: Vec<u8>,
    }

    #[derive(Debug)]
    struct FfiCancellableEncodedProxy {
        cancelled: bool,
        proxy: FfiEncodedProxy,
        mask_coverage: FfiEditPreviewMaskCoverage,
    }

    #[derive(Debug)]
    struct FfiCancellableAnalyzedEditPreview {
        cancelled: bool,
        preview: FfiAnalyzedEditPreview,
        mask_coverage: FfiEditPreviewMaskCoverage,
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

    #[derive(Debug, Clone, Copy)]
    struct FfiPhotoLiquifyPoint {
        x: f64,
        y: f64,
        pressure: f64,
    }

    /// Flat, explicitly present structural-node payload. Stroke parameters
    /// contain radius/strength/hardness triples in stroke order; point counts
    /// partition the single authored-point vector without nested bridge
    /// allocation.
    #[derive(Debug)]
    struct FfiPhotoLiquify {
        present: bool,
        points: Vec<FfiPhotoLiquifyPoint>,
        stroke_point_counts: Vec<u32>,
        stroke_parameters: Vec<f64>,
    }

    #[derive(Debug)]
    struct FfiAdjustmentRenderRequest {
        nodes: Vec<FfiAdjustmentNode>,
        liquify: FfiPhotoLiquify,
        geometry: FfiPhotoGeometry,
        max_edge: u32,
        jpeg_quality: u8,
        mask_coverage_requested: bool,
        mask_coverage_target_layer_index: u32,
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
        liquify: FfiPhotoLiquify,
        geometry: FfiPhotoGeometry,
        rect: FfiDetailTileRect,
    }

    /// Runtime-only source admission requirements for a prepared full-detail
    /// session. These flags never enter Recipe persistence or image identity.
    #[derive(Debug, Clone, Copy)]
    struct FfiDetailSessionRequirements {
        requires_cpu_replay: bool,
    }

    #[derive(Debug)]
    struct FfiRenderedDetailTile {
        rect: FfiDetailTileRect,
        full_dimensions: FfiDimensions,
        row_stride_bytes: u32,
        bytes: Vec<u8>,
        execution_backend: u8,
        execution_backend_version: u32,
        source_cache_hit: bool,
        fell_back: bool,
        diagnostic: String,
    }

    unsafe extern "C++" {
        include!("shadow/image/cxx_bridge.hpp");

        type DecodeHandle;
        type EditPreviewHandle;
        type EditPreviewCancellationHandle;
        type InteractiveEditPreviewFrameHandle;
        type FullEditDetailHandle;

        fn open_libraw_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn open_photo_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn query_libraw_optics_profiles_utf8(path: &str) -> Result<Vec<FfiOpticsProfileCandidate>>;
        fn query_photo_optics_profiles_utf8(path: &str) -> Result<Vec<FfiOpticsProfileCandidate>>;
        fn query_optics_profiles_for_metadata(
            metadata: &FfiMetadataSnapshot,
        ) -> Vec<FfiOpticsProfileCandidate>;
        fn query_raw_white_balance_presentation_for_metadata(
            metadata: &FfiMetadataSnapshot,
        ) -> FfiRawWhiteBalancePresentation;
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
        fn prepare_edit_preview_with_raw_foundation(
            self: &DecodeHandle,
            max_edge: u32,
            plan: &FfiRawDevelopmentPlan,
            foundation: &FfiRawFoundation,
        ) -> Result<UniquePtr<EditPreviewHandle>>;
        #[allow(dead_code)]
        fn prepare_edit_detail(self: &DecodeHandle) -> Result<UniquePtr<FullEditDetailHandle>>;
        fn prepare_edit_detail_with_raw_development_plan(
            self: &DecodeHandle,
            plan: &FfiRawDevelopmentPlan,
            requirements: &FfiDetailSessionRequirements,
        ) -> Result<UniquePtr<FullEditDetailHandle>>;
        fn prepare_edit_detail_with_raw_foundation(
            self: &DecodeHandle,
            plan: &FfiRawDevelopmentPlan,
            foundation: &FfiRawFoundation,
            requirements: &FfiDetailSessionRequirements,
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
        fn render_adjustment_plan_rgb8_cancellable(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
            cancellation: &EditPreviewCancellationHandle,
        ) -> Result<FfiCancellableEncodedProxy>;
        fn render_adjustment_plan_owned_rgb8_cancellable(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
            cancellation: &EditPreviewCancellationHandle,
        ) -> Result<UniquePtr<InteractiveEditPreviewFrameHandle>>;
        fn width(self: &InteractiveEditPreviewFrameHandle) -> u32;
        fn height(self: &InteractiveEditPreviewFrameHandle) -> u32;
        fn row_stride_bytes(self: &InteractiveEditPreviewFrameHandle) -> u32;
        fn storage_kind(self: &InteractiveEditPreviewFrameHandle) -> u8;
        fn native_texture_row_stride_bytes(self: &InteractiveEditPreviewFrameHandle) -> u32;
        fn native_texture_pixel_format(self: &InteractiveEditPreviewFrameHandle) -> u8;
        fn native_resource_id(self: &InteractiveEditPreviewFrameHandle) -> u64;
        fn native_texture_handle(self: &InteractiveEditPreviewFrameHandle) -> usize;
        fn native_device_handle(self: &InteractiveEditPreviewFrameHandle) -> usize;
        fn materialized_pixel_bytes(self: &InteractiveEditPreviewFrameHandle) -> usize;
        fn retained_bytes(self: &InteractiveEditPreviewFrameHandle) -> usize;
        fn presentation_fallback_diagnostic(self: &InteractiveEditPreviewFrameHandle) -> String;
        fn materialize_pixels<'a>(self: &'a InteractiveEditPreviewFrameHandle) -> Result<&'a [u8]>;
        fn mask_coverage_available(self: &InteractiveEditPreviewFrameHandle) -> bool;
        fn mask_coverage_version(self: &InteractiveEditPreviewFrameHandle) -> String;
        fn mask_coverage_layer_index(self: &InteractiveEditPreviewFrameHandle) -> u32;
        fn mask_coverage_width(self: &InteractiveEditPreviewFrameHandle) -> u32;
        fn mask_coverage_height(self: &InteractiveEditPreviewFrameHandle) -> u32;
        fn mask_coverage_row_stride_bytes(self: &InteractiveEditPreviewFrameHandle) -> u32;
        fn mask_coverage_samples<'a>(self: &'a InteractiveEditPreviewFrameHandle) -> &'a [u8];
        fn render_adjustment_plan_with_analysis_cancellable(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
            cancellation: &EditPreviewCancellationHandle,
        ) -> Result<FfiCancellableAnalyzedEditPreview>;
        fn dimensions(self: &FullEditDetailHandle) -> FfiDimensions;
        fn cpu_replay_available(self: &FullEditDetailHandle) -> bool;
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

#[cfg(test)]
mod tests;
