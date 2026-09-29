//! Reusable warm-preview state, cancellation, and session rendering.

use std::path::Path;

use shadow_domain::{ImageDimensions, RawMetadataSnapshot};

use super::{
    BridgeError,
    adjustment::{
        AdjustmentRenderPlan, BasicEditParameters, basic_adjustment_render_plan,
        validate_jpeg_quality, validate_warm_edit_max_edge,
    },
    decoder::{dimensions, open_photo},
    ffi,
    optics::{
        OpticsReceipt, OpticsSettings, ffi_metadata_snapshot, ffi_optics_settings, optics_receipt,
    },
    preview_analysis::{
        AnalyzedEditPreview, EditPreviewMaskCoverageRequest, RenderedEditPreview,
        SensorClippingMask, validate_analyzed_edit_preview, validate_mask_coverage,
        validate_mask_coverage_request, validate_rgb8_edit_preview, validate_sensor_clipping_mask,
    },
    preview_frame::OwnedInteractivePreviewFrame,
    raw_development::{
        RawDevelopmentIntent, RawDevelopmentPlan, RawDevelopmentReceipt, RawPipelineReceipt,
        ffi_raw_development_plan, preflight_photo_edit_development, raw_development_receipt,
        raw_pipeline_receipt,
    },
    raw_foundation::VerifiedRawFoundation,
    render_wire::{ffi_render_request, ffi_render_request_with_mask_coverage, proxy_payload},
};

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

/// Cache-key version for the fixed-order basic edited-preview recipe.
pub const BASIC_EDIT_PREVIEW_RECIPE_VERSION: u32 = 1;

/// Hard memory bound for the reusable float working proxy.
///
/// A square proxy at this edge consumes at most 192 MiB for interleaved RGB
/// float32. The intended UI values are 1600 and 2048.
pub const MAX_WARM_EDIT_PREVIEW_EDGE: u32 = 4_096;

#[derive(Debug, Clone)]
pub struct CurveInputMap {
    pub width: u32,
    pub height: u32,
    pub values: Vec<f32>,
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
    level_zero_dimensions: ImageDimensions,
    max_edge: u32,
    sensor_clipping_mask: SensorClippingMask,
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

impl std::fmt::Debug for LibRawEditPreviewSession {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LibRawEditPreviewSession")
            .field("dimensions", &self.dimensions)
            .field("level_zero_dimensions", &self.level_zero_dimensions)
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
    ///
    /// # Errors
    ///
    /// Returns a plan-validation, path, decoder, resource-limit, or invalid
    /// bridge-output error when preparation fails.
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
    ///
    /// # Errors
    ///
    /// Returns a plan/optics validation, path, decoder, resource-limit, or
    /// invalid bridge-output error when preparation fails.
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
        Self::from_prepared_handle(handle)
    }

    /// Opens a preview from one completely verified, path-free AI RAW foundation.
    ///
    /// The foundation buffer remains Rust-owned and is borrowed by C++ only while the immutable
    /// native preview session is prepared. Enabled AI preparation is fail-closed and never
    /// substitutes provider RGB or ordinary Bayer reconstruction.
    ///
    /// # Errors
    ///
    /// Returns a foundation/plan/optics/path/decoder/resource-limit or invalid-output error.
    pub fn open_with_raw_foundation(
        path: &Path,
        max_edge: u32,
        raw_development_plan: RawDevelopmentPlan,
        foundation: &VerifiedRawFoundation,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        validate_warm_edit_max_edge(max_edge)?;
        raw_development_plan.validate()?;
        if raw_development_plan.intent != RawDevelopmentIntent::Preview {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "AI RAW foundation warm previews require preview RAW-development intent",
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
        let handle = decode_handle.prepare_edit_preview_with_raw_foundation(
            max_edge,
            &ffi_raw_development_plan(raw_development_plan),
            foundation.ffi(),
        )?;
        Self::from_prepared_handle(handle)
    }

    /// Opens an ordinary RAW preview from a provider-neutral `RawFrame`
    /// staged by the isolated decoder helper. The original decode session is
    /// used only for metadata, profiles, and optics; the returned warm session
    /// retains its own bounded camera basis.
    ///
    /// # Errors
    ///
    /// Returns a staging, plan, optics, path, decoder, resource, admission, or
    /// invalid-output error.
    pub fn open_with_staged_raw_development_plan(
        path: &Path,
        staging_manifest_path: &Path,
        max_edge: u32,
        raw_development_plan: RawDevelopmentPlan,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        validate_warm_edit_max_edge(max_edge)?;
        raw_development_plan.validate()?;
        if raw_development_plan.intent != RawDevelopmentIntent::Preview {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "staged RAW warm previews require preview RAW-development intent",
            ));
        }
        let staging_manifest = staging_manifest_path
            .to_str()
            .ok_or_else(|| BridgeError::NonUtf8Path(staging_manifest_path.to_path_buf()))?;
        let mut decode_handle = open_photo(path)?;
        if decode_handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        decode_handle
            .pin_mut()
            .configure_optics(&ffi_optics_settings(optics))?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_preview_with_staged_raw_development_plan(
            max_edge,
            &ffi_raw_development_plan(raw_development_plan),
            staging_manifest,
        )?;
        Self::from_prepared_handle(handle)
    }

    /// Opens a rebindable RAW preview from the isolated decoder helper's
    /// paired `RawFrame` and metadata snapshot. Unlike the legacy staged entry
    /// point, this never reopens the original source path in the desktop
    /// process.
    ///
    /// # Errors
    ///
    /// Returns a staging, metadata, plan, optics, resource, admission, or
    /// invalid-output error.
    pub fn open_with_staged_raw_development_plan_from_metadata(
        metadata: &RawMetadataSnapshot,
        staging_manifest_path: &Path,
        max_edge: u32,
        raw_development_plan: RawDevelopmentPlan,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        validate_warm_edit_max_edge(max_edge)?;
        raw_development_plan.validate()?;
        if raw_development_plan.intent != RawDevelopmentIntent::Preview {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "staged RAW warm previews require preview RAW-development intent",
            ));
        }
        let staging_manifest = staging_manifest_path
            .to_str()
            .ok_or_else(|| BridgeError::NonUtf8Path(staging_manifest_path.to_path_buf()))?;
        let handle = ffi::prepare_edit_preview_with_staged_raw_development_plan_from_metadata(
            &ffi_metadata_snapshot(metadata),
            max_edge,
            &ffi_raw_development_plan(raw_development_plan),
            staging_manifest,
            &ffi_optics_settings(optics),
        )?;
        Self::from_prepared_handle(handle)
    }

    /// Opens a preview from one verified foundation and the exact
    /// provider-neutral `RawFrame` staged by the isolated decoder transaction.
    /// The staging files are borrowed only during synchronous native
    /// preparation; the returned warm session retains a bounded camera basis.
    ///
    /// # Errors
    ///
    /// Returns a staging, foundation, plan, optics, path, decoder, resource,
    /// or invalid-output error.
    pub fn open_with_staged_raw_foundation(
        path: &Path,
        staging_manifest_path: &Path,
        max_edge: u32,
        raw_development_plan: RawDevelopmentPlan,
        foundation: &VerifiedRawFoundation,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        validate_warm_edit_max_edge(max_edge)?;
        raw_development_plan.validate()?;
        if raw_development_plan.intent != RawDevelopmentIntent::Preview {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "staged AI RAW foundation warm previews require preview RAW-development intent",
            ));
        }
        let staging_manifest = staging_manifest_path
            .to_str()
            .ok_or_else(|| BridgeError::NonUtf8Path(staging_manifest_path.to_path_buf()))?;
        let mut decode_handle = open_photo(path)?;
        if decode_handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        decode_handle
            .pin_mut()
            .configure_optics(&ffi_optics_settings(optics))?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_preview_with_staged_raw_foundation(
            max_edge,
            &ffi_raw_development_plan(raw_development_plan),
            foundation.ffi(),
            staging_manifest,
        )?;
        Self::from_prepared_handle(handle)
    }

    fn from_prepared_handle(
        handle: cxx::UniquePtr<ffi::EditPreviewHandle>,
    ) -> Result<Self, BridgeError> {
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let level_zero_dimensions = dimensions(&prepared.level_zero_dimensions());
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
            level_zero_dimensions,
            max_edge: prepared_max_edge,
            sensor_clipping_mask,
            raw_development_receipt,
            raw_pipeline_receipt,
            optics_receipt,
        })
    }

    /// Reads a bounded, transient map from an already compiled curve-input prefix.
    pub fn curve_input_map(
        &self,
        plan: &AdjustmentRenderPlan,
        channel: u8,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CurveInputMap, BridgeError> {
        plan.validate()?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let cancel = cancellation
            .handle
            .as_ref()
            .ok_or(BridgeError::NullHandle)?;
        let map = handle.curve_input_map(
            &ffi_render_request(plan, self.max_edge, 90),
            channel,
            cancel,
        )?;
        Ok(CurveInputMap {
            width: map.width,
            height: map.height,
            values: map.values,
        })
    }

    /// Returns whether this session retained an immutable RAW camera-space basis that can be
    /// rebound to a different absolute white balance without reopening the source or repeating
    /// sensor-domain denoise.
    #[must_use]
    pub fn supports_raw_development_rebinding(&self) -> bool {
        self.handle
            .as_ref()
            .is_some_and(ffi::EditPreviewHandle::supports_raw_development_rebinding)
    }

    /// Reports whether this session retained a sensor-domain basis suitable
    /// for a true RAW neutral picker. Compatibility RGB sources deliberately
    /// return false rather than inferring a camera white point from display
    /// pixels.
    #[must_use]
    pub fn supports_raw_white_balance_picker(&self) -> bool {
        self.handle
            .as_ref()
            .is_some_and(ffi::EditPreviewHandle::supports_raw_white_balance_picker)
    }

    /// Samples the retained CFA source at a normalized display coordinate and
    /// returns the calibrated photographic RAW white point when available.
    #[must_use]
    pub fn pick_raw_white_balance(
        &self,
        normalized_x: f64,
        normalized_y: f64,
    ) -> Option<(u32, i16)> {
        if !(0.0..=1.0).contains(&normalized_x) || !(0.0..=1.0).contains(&normalized_y) {
            return None;
        }
        let handle = self.handle.as_ref()?;
        let presentation = handle.pick_raw_white_balance(normalized_x, normalized_y);
        presentation
            .available
            .then_some((presentation.temperature_kelvin, presentation.tint))
    }

    /// Estimates one neutral white point from a bounded grid over the exact
    /// retained CFA source. This never prepares or decodes another source.
    #[must_use]
    pub fn auto_raw_white_balance(&self) -> Option<(u32, i16)> {
        let handle = self.handle.as_ref()?;
        let presentation = handle.auto_raw_white_balance();
        presentation
            .available
            .then_some((presentation.temperature_kelvin, presentation.tint))
    }

    /// Creates a new immutable preview session over the same decoded/denoised RAW basis.
    ///
    /// Only white balance may differ from the source session's request. The returned session owns
    /// fresh colour/DCP provenance and GPU edit state; the source session remains usable.
    ///
    /// # Errors
    ///
    /// Returns an error when the plan is invalid or is not a preview request, when the source
    /// session has no native handle, or when native RAW rebinding cannot prepare a new session.
    pub fn rebind_raw_development_plan(
        &self,
        raw_development_plan: RawDevelopmentPlan,
    ) -> Result<Self, BridgeError> {
        raw_development_plan.validate()?;
        if raw_development_plan.intent != RawDevelopmentIntent::Preview {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "warm edit preview rebinding requires preview RAW-development intent",
            ));
        }
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let rebound =
            handle.rebind_raw_development_plan(&ffi_raw_development_plan(raw_development_plan))?;
        Self::from_prepared_handle(rebound)
    }

    /// Returns whether this AI preview retained paired bounded original/foundation camera-RGB
    /// bases for amount-only rebinding.
    #[must_use]
    pub fn supports_raw_foundation_amount_rebinding(&self) -> bool {
        self.handle
            .as_ref()
            .is_some_and(ffi::EditPreviewHandle::supports_raw_foundation_amount_rebinding)
    }

    /// Creates a new immutable preview session over the same bounded original/AI bases.
    ///
    /// This performs no source decode, foundation-artifact read, or model execution. White balance
    /// may change in the same transaction so one slider generation cannot bind two source states.
    ///
    /// # Errors
    ///
    /// Returns an error for an invalid plan/amount, a non-AI source, or a session that did not
    /// retain the paired amount basis.
    pub fn rebind_raw_foundation_amount(
        &self,
        raw_development_plan: RawDevelopmentPlan,
        amount_percent: u8,
    ) -> Result<Self, BridgeError> {
        raw_development_plan.validate()?;
        if raw_development_plan.intent != RawDevelopmentIntent::Preview {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "AI foundation amount rebinding requires preview RAW-development intent",
            ));
        }
        if amount_percent > 100 {
            return Err(BridgeError::InvalidRawFoundation(
                "amount must be between 0 and 100 percent",
            ));
        }
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let rebound = handle.rebind_raw_foundation_amount(
            &ffi_raw_development_plan(raw_development_plan),
            amount_percent,
        )?;
        Self::from_prepared_handle(rebound)
    }

    /// Returns the fixed pixel dimensions of every preview from this session.
    #[must_use]
    pub const fn dimensions(&self) -> ImageDimensions {
        self.dimensions
    }

    /// Returns the oriented level-zero source dimensions used by pixel-sized edit parameters.
    #[must_use]
    pub const fn level_zero_dimensions(&self) -> ImageDimensions {
        self.level_zero_dimensions
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
    ///
    /// # Errors
    ///
    /// Returns an invalid-request, decoder, null-handle, or invalid-output
    /// error when a non-cancelled render cannot complete its contract.
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
        validate_mask_coverage(rendered.mask_coverage, None, output_dimensions)?;
        let proxy = proxy_payload(rendered.proxy);
        if proxy.dimensions != output_dimensions {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "geometry-aware preview dimensions do not match the rendered canvas",
            ));
        }
        Ok(CancellableEditPreview::Completed(proxy))
    }

    /// Executes a typed plan with cooperative native cancellation and returns
    /// tightly packed display-sRGB RGB8 pixels without JPEG encoding.
    ///
    /// This is the transient presentation route used while a control gesture
    /// is active. It is intentionally separate from cacheable JPEG output.
    ///
    /// # Errors
    ///
    /// Returns an invalid-request, decoder, null-handle, or invalid-output
    /// error when a non-cancelled render cannot complete its RGB8 contract.
    pub fn render_plan_rgb8_cancellable(
        &self,
        plan: &AdjustmentRenderPlan,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CancellableEditPreview<shadow_domain::ProxyPayload>, BridgeError> {
        match self.render_plan_rgb8_with_mask_coverage_cancellable(plan, None, cancellation)? {
            CancellableEditPreview::Completed(rendered) => {
                Ok(CancellableEditPreview::Completed(rendered.proxy))
            }
            CancellableEditPreview::Cancelled => Ok(CancellableEditPreview::Cancelled),
        }
    }

    /// Executes a typed plan and optionally captures one Grade Node's exact
    /// local-mask coverage from the same cancellable RGB8 render.
    ///
    /// The target is a zero-based compiled layer index. Selection revision is
    /// returned unchanged as host transaction metadata and never enters native
    /// evaluation or durable cache identity. Cancellation returns neither the
    /// preview nor coverage half.
    ///
    /// # Errors
    ///
    /// Returns an invalid-request error for an out-of-range target, or an
    /// invalid-output error if native coverage is unpaired, malformed, stale,
    /// padded, or uses an unsupported version.
    pub fn render_plan_rgb8_with_mask_coverage_cancellable(
        &self,
        plan: &AdjustmentRenderPlan,
        mask_coverage: Option<EditPreviewMaskCoverageRequest>,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CancellableEditPreview<RenderedEditPreview>, BridgeError> {
        plan.validate()?;
        if let Some(request) = mask_coverage {
            validate_mask_coverage_request(plan, request)?;
        }
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let cancellation = cancellation
            .handle
            .as_ref()
            .ok_or(BridgeError::NullHandle)?;
        // The wire request still contains the legacy JPEG-quality field, but
        // the RGB8 native entry point does not inspect it.
        let request = ffi_render_request_with_mask_coverage(plan, self.max_edge, 95, mask_coverage);
        let rendered = handle.render_adjustment_plan_rgb8_cancellable(&request, cancellation)?;
        if rendered.cancelled {
            return Ok(CancellableEditPreview::Cancelled);
        }
        let proxy = validate_rgb8_edit_preview(proxy_payload(rendered.proxy), output_dimensions)?;
        let mask_coverage =
            validate_mask_coverage(rendered.mask_coverage, mask_coverage, output_dimensions)?;
        Ok(CancellableEditPreview::Completed(RenderedEditPreview {
            proxy,
            mask_coverage,
        }))
    }

    /// Executes a typed plan into one move-only native RGB8 frame owner with optional paired
    /// local-mask coverage.
    ///
    /// Unlike [`Self::render_plan_rgb8_with_mask_coverage_cancellable`], this interactive route
    /// does not copy either full-frame vector into a Rust allocation. Returned slices borrow the
    /// completed frame and remain valid after this session is dropped. Cooperative cancellation
    /// publishes no owner and returns [`CancellableEditPreview::Cancelled`].
    ///
    /// # Errors
    ///
    /// Returns an invalid-request, decoder, or invalid-output error when a non-cancelled render
    /// cannot produce one complete, tightly packed RGB8/coverage owner.
    pub fn render_plan_interactive_frame_cancellable(
        &self,
        plan: &AdjustmentRenderPlan,
        mask_coverage: Option<EditPreviewMaskCoverageRequest>,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CancellableEditPreview<OwnedInteractivePreviewFrame>, BridgeError> {
        plan.validate()?;
        if let Some(request) = mask_coverage {
            validate_mask_coverage_request(plan, request)?;
        }
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let cancellation = cancellation
            .handle
            .as_ref()
            .ok_or(BridgeError::NullHandle)?;
        // The wire request retains the compatibility JPEG-quality field; the owned RGB8 native
        // entry point does not inspect it.
        let request = ffi_render_request_with_mask_coverage(plan, self.max_edge, 95, mask_coverage);
        let native =
            handle.render_adjustment_plan_owned_rgb8_cancellable(&request, cancellation)?;
        match OwnedInteractivePreviewFrame::from_nullable_native(
            native,
            output_dimensions,
            mask_coverage,
        )? {
            Some(frame) => Ok(CancellableEditPreview::Completed(frame)),
            None => Ok(CancellableEditPreview::Cancelled),
        }
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
    ///
    /// # Errors
    ///
    /// Returns an invalid-request, decoder, null-handle, or invalid-output
    /// error when a non-cancelled render cannot complete its contract.
    pub fn render_plan_with_analysis_cancellable(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CancellableEditPreview<AnalyzedEditPreview>, BridgeError> {
        self.render_plan_with_analysis_and_mask_coverage_cancellable(
            plan,
            jpeg_quality,
            None,
            cancellation,
        )
    }

    /// Executes a typed plan with generation-matched analysis and optional
    /// exact local-mask coverage under one cooperative cancellation terminal.
    ///
    /// # Errors
    ///
    /// Returns an invalid request for an out-of-range coverage target, or an
    /// invalid-output error when any completed proxy, analysis, execution
    /// receipt, or coverage field violates its paired contract.
    pub fn render_plan_with_analysis_and_mask_coverage_cancellable(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
        mask_coverage: Option<EditPreviewMaskCoverageRequest>,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CancellableEditPreview<AnalyzedEditPreview>, BridgeError> {
        plan.validate()?;
        validate_jpeg_quality(jpeg_quality)?;
        if let Some(request) = mask_coverage {
            validate_mask_coverage_request(plan, request)?;
        }
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let cancellation = cancellation
            .handle
            .as_ref()
            .ok_or(BridgeError::NullHandle)?;
        let request =
            ffi_render_request_with_mask_coverage(plan, self.max_edge, jpeg_quality, mask_coverage);
        let rendered =
            handle.render_adjustment_plan_with_analysis_cancellable(&request, cancellation)?;
        if rendered.cancelled {
            return Ok(CancellableEditPreview::Cancelled);
        }
        let analyzed = rendered.preview;
        let proxy = proxy_payload(analyzed.proxy);
        let mut completed = validate_analyzed_edit_preview(
            proxy,
            analyzed.analysis,
            analyzed.execution,
            output_dimensions,
        )?;
        completed.mask_coverage =
            validate_mask_coverage(rendered.mask_coverage, mask_coverage, output_dimensions)?;
        Ok(CancellableEditPreview::Completed(completed))
    }
}
