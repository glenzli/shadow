//! Full-resolution detail-tile state, request bounds, and output validation.

use std::path::Path;

use shadow_domain::ImageDimensions;

use super::{
    BridgeError,
    adjustment::{AdjustmentLocalMask, AdjustmentRenderOperation, AdjustmentRenderPlan},
    decoder::{dimensions, open_photo},
    ffi,
    optics::{OpticsReceipt, OpticsSettings, ffi_optics_settings, optics_receipt},
    raw_development::{
        RawDevelopmentIntent, RawDevelopmentPlan, RawDevelopmentReceipt, RawPipelineReceipt,
        ffi_raw_development_plan, preflight_photo_edit_development, raw_development_receipt,
        raw_pipeline_receipt,
    },
    raw_foundation::VerifiedRawFoundation,
    render_wire::{detail_tile_rect, ffi_detail_tile_request},
};

// SAFETY: the C++ handle owns a fully prepared, immutable linear reference image and a
// mutex-protected bounded GPU cache. It contains no decoder or borrowed state, and every render
// owns its output buffers. The public wrapper exposes no mutable access to the handle.
unsafe impl Send for ffi::FullEditDetailHandle {}
// SAFETY: see the Send implementation above. Concurrent calls only read the retained source.
unsafe impl Sync for ffi::FullEditDetailHandle {}

/// Hard width and height bound for one full-resolution detail tile.
pub const MAX_EDIT_DETAIL_TILE_SIDE: u32 = 1_024;

/// Hard bound for the largest complete immutable source retained by one detail session.
///
/// Packed u16 raster sources retain at most 512 MiB. Owned `RawFrame` development retains fp32
/// scene-linear RGB and is independently capped at 1 GiB by the native session.
pub const MAX_EDIT_DETAIL_RETAINED_BYTES: u64 = 1_024 * 1_024 * 1_024;

/// Runtime source capabilities required by a complete full-detail render plan.
///
/// This is execution policy, not authored Recipe state. The current Metal
/// structural path does not implement Liquify or composite local masks, so
/// those plans require a retained source that can be replayed through the
/// portable CPU executor. Once Metal reaches parity, this derivation can admit
/// a Metal-resident source without changing Recipe or render-plan schemas.
#[derive(Debug, Clone, Copy, Default, Eq, PartialEq)]
pub struct DetailSessionRequirements {
    requires_cpu_replay: bool,
}

impl DetailSessionRequirements {
    /// Derives source-admission requirements from the complete executable plan.
    #[must_use]
    pub fn for_render_plan(plan: &AdjustmentRenderPlan) -> Self {
        let requires_cpu_replay = plan.liquify.is_some()
            || plan.nodes.iter().any(|node| {
                matches!(
                    &node.operation,
                    AdjustmentRenderOperation::LocalMaskLayerStart {
                        mask: Some(AdjustmentLocalMask::Composite { .. }),
                        ..
                    }
                )
            });
        Self {
            requires_cpu_replay,
        }
    }

    /// Derives source admission for a high-bit export. RGB16 currently uses
    /// the portable CPU display boundary, so it must not retain a Metal-only
    /// source even when the authored plan has no other CPU-only stage.
    #[must_use]
    pub const fn for_high_bit_export(_plan: &AdjustmentRenderPlan) -> Self {
        Self {
            requires_cpu_replay: true,
        }
    }

    /// Whether the retained source must support complete CPU replay.
    #[must_use]
    pub const fn requires_cpu_replay(self) -> bool {
        self.requires_cpu_replay
    }
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
    pub(crate) fn validate(self, full_dimensions: ImageDimensions) -> Result<(), BridgeError> {
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
    pub execution: DetailTileExecutionReceipt,
}

/// Packed host-endian, display-encoded sRGB RGB16 samples for one exact
/// full-resolution rectangle.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RenderedDetailTile16 {
    pub rect: DetailTileRect,
    pub full_dimensions: ImageDimensions,
    pub row_stride_bytes: u32,
    pub samples: Vec<u16>,
    pub execution: DetailTileExecutionReceipt,
}

/// Effective complete adjustment-plus-display backend for one detail tile.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum DetailTileRenderBackend {
    Cpu,
    Metal,
}

/// Runtime-only execution provenance. `source_cache_hit` reports whether an expanded working
/// tile was already resident on the GPU; it does not participate in recipe or durable cache
/// identities.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct DetailTileExecutionReceipt {
    pub backend: DetailTileRenderBackend,
    pub backend_version: u32,
    pub source_cache_hit: bool,
    pub fell_back: bool,
    pub diagnostic: Option<String>,
}

fn detail_execution_receipt(
    execution_backend: u8,
    execution_backend_version: u32,
    source_cache_hit: bool,
    fell_back: bool,
    diagnostic: String,
) -> Result<DetailTileExecutionReceipt, BridgeError> {
    let backend = match execution_backend {
        0 => DetailTileRenderBackend::Cpu,
        1 => DetailTileRenderBackend::Metal,
        _ => {
            return Err(BridgeError::InvalidEditDetailOutput(
                "detail execution backend is unknown",
            ));
        }
    };
    let diagnostic = (!diagnostic.is_empty()).then_some(diagnostic);
    if execution_backend_version != 1
        || (backend == DetailTileRenderBackend::Cpu && source_cache_hit)
        || (backend == DetailTileRenderBackend::Metal && fell_back)
        || fell_back != diagnostic.is_some()
    {
        return Err(BridgeError::InvalidEditDetailOutput(
            "detail execution receipt is inconsistent",
        ));
    }
    Ok(DetailTileExecutionReceipt {
        backend,
        backend_version: execution_backend_version,
        source_cache_hit,
        fell_back,
        diagnostic,
    })
}

/// A reusable immutable full-resolution linear RGB source in sRGB primaries for 1:1 tiles.
///
/// Preparation performs one source-router reference render, retains no decoder, and fails when
/// either the metadata worst-case RGB allocation or the actual retained allocation exceeds its
/// native source-kind limit. Repeated tile renders convert and edit only the requested rectangle.
/// The wrapper is [`Send`] + [`Sync`], and concurrent renders own independent temporary buffers.
pub struct LibRawEditDetailSession {
    handle: cxx::UniquePtr<ffi::FullEditDetailHandle>,
    dimensions: ImageDimensions,
    retained_bytes: u64,
    cpu_replay_available: bool,
    raw_development_receipt: RawDevelopmentReceipt,
    raw_pipeline_receipt: RawPipelineReceipt,
    optics_receipt: OpticsReceipt,
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
            .field("cpu_replay_available", &self.cpu_replay_available)
            .field("raw_development_receipt", &self.raw_development_receipt)
            .field("raw_pipeline_receipt", &self.raw_pipeline_receipt)
            .field("optics_receipt", &self.optics_receipt)
            .finish_non_exhaustive()
    }
}

impl LibRawEditDetailSession {
    /// Opens a supported photo into immutable full-resolution linear RGB pixels in sRGB
    /// primaries. Raster compatibility sources use packed u16; owned RAW development retains
    /// scene-linear fp32.
    ///
    /// Provider metadata is checked against the worst-case RGB retention limit before the
    /// reference render starts. The returned allocation is checked independently against its
    /// source-kind limit before it is retained by the session.
    ///
    /// # Errors
    ///
    /// Returns a path, decoder, resource-limit, or invalid bridge-output error. Sources whose
    /// worst-case or actual retained allocation exceeds the native source-kind cap fail closed.
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
    /// `Detail` and `ExportImage` intents are accepted; `Preview` is rejected so a warm half-size
    /// source can never enter a full-resolution pipeline.
    ///
    /// # Errors
    ///
    /// Returns a plan-validation, path, decoder, resource-limit, or invalid
    /// bridge-output error when preparation fails.
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
    ///
    /// # Errors
    ///
    /// Returns a plan/optics validation, path, decoder, resource-limit, or
    /// invalid bridge-output error when preparation fails.
    pub fn open_with_raw_development_plan_and_optics(
        path: &Path,
        raw_development_plan: RawDevelopmentPlan,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        Self::open_with_requirements(
            path,
            raw_development_plan,
            optics,
            DetailSessionRequirements::default(),
        )
    }

    /// Opens a native-detail session after negotiating the source capabilities
    /// required by the complete render plan.
    ///
    /// Requirement satisfaction is checked again on the prepared native
    /// session before it becomes observable to callers.
    ///
    /// # Errors
    ///
    /// Returns a plan/optics validation, path, decoder, resource-limit,
    /// admission, or invalid bridge-output error when preparation fails.
    pub fn open_with_requirements(
        path: &Path,
        raw_development_plan: RawDevelopmentPlan,
        optics: &OpticsSettings,
        requirements: DetailSessionRequirements,
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
            &ffi::FfiDetailSessionRequirements {
                requires_cpu_replay: requirements.requires_cpu_replay(),
            },
        )?;
        Self::from_prepared_handle(handle, requirements)
    }

    /// Opens full detail/export from one fully verified AI RAW foundation.
    ///
    /// The large interleaved camera-RGB vector is borrowed synchronously and never becomes part
    /// of the returned handle. Native preparation retains only the independently owned
    /// scene-linear result, so the foundation cannot enter the resident-CFA route.
    ///
    /// # Errors
    ///
    /// Returns a foundation/plan/optics/path/decoder/resource-limit, admission, or
    /// invalid-output error.
    pub fn open_with_raw_foundation(
        path: &Path,
        raw_development_plan: RawDevelopmentPlan,
        foundation: &VerifiedRawFoundation,
        optics: &OpticsSettings,
        requirements: DetailSessionRequirements,
    ) -> Result<Self, BridgeError> {
        raw_development_plan.validate()?;
        if !matches!(
            raw_development_plan.intent,
            RawDevelopmentIntent::Detail | RawDevelopmentIntent::ExportImage
        ) {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "AI RAW foundation full resolution requires detail or export-image intent",
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
        let handle = decode_handle.prepare_edit_detail_with_raw_foundation(
            &ffi_raw_development_plan(raw_development_plan),
            foundation.ffi(),
            &ffi::FfiDetailSessionRequirements {
                requires_cpu_replay: requirements.requires_cpu_replay(),
            },
        )?;
        Self::from_prepared_handle(handle, requirements)
    }

    /// Opens full detail/export from an ordinary provider-neutral `RawFrame`
    /// staged by the isolated decoder helper.
    ///
    /// # Errors
    ///
    /// Returns a staging, plan, optics, path, decoder, resource, admission, or
    /// invalid-output error.
    pub fn open_with_staged_raw_development_plan(
        path: &Path,
        staging_manifest_path: &Path,
        raw_development_plan: RawDevelopmentPlan,
        optics: &OpticsSettings,
        requirements: DetailSessionRequirements,
    ) -> Result<Self, BridgeError> {
        raw_development_plan.validate()?;
        if !matches!(
            raw_development_plan.intent,
            RawDevelopmentIntent::Detail | RawDevelopmentIntent::ExportImage
        ) {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "staged RAW full resolution requires detail or export-image intent",
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
        let handle = decode_handle.prepare_edit_detail_with_staged_raw_development_plan(
            &ffi_raw_development_plan(raw_development_plan),
            staging_manifest,
            &ffi::FfiDetailSessionRequirements {
                requires_cpu_replay: requirements.requires_cpu_replay(),
            },
        )?;
        Self::from_prepared_handle(handle, requirements)
    }

    /// Opens full detail/export from a verified foundation and the exact
    /// provider-neutral `RawFrame` staged by the isolated decoder transaction.
    ///
    /// # Errors
    ///
    /// Returns a staging, foundation, plan, optics, path, decoder, resource,
    /// admission, or invalid-output error.
    pub fn open_with_staged_raw_foundation(
        path: &Path,
        staging_manifest_path: &Path,
        raw_development_plan: RawDevelopmentPlan,
        foundation: &VerifiedRawFoundation,
        optics: &OpticsSettings,
        requirements: DetailSessionRequirements,
    ) -> Result<Self, BridgeError> {
        raw_development_plan.validate()?;
        if !matches!(
            raw_development_plan.intent,
            RawDevelopmentIntent::Detail | RawDevelopmentIntent::ExportImage
        ) {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "staged AI RAW foundation full resolution requires detail or export-image intent",
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
        let handle = decode_handle.prepare_edit_detail_with_staged_raw_foundation(
            &ffi_raw_development_plan(raw_development_plan),
            foundation.ffi(),
            staging_manifest,
            &ffi::FfiDetailSessionRequirements {
                requires_cpu_replay: requirements.requires_cpu_replay(),
            },
        )?;
        Self::from_prepared_handle(handle, requirements)
    }

    fn from_prepared_handle(
        handle: cxx::UniquePtr<ffi::FullEditDetailHandle>,
        requirements: DetailSessionRequirements,
    ) -> Result<Self, BridgeError> {
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let retained_bytes = prepared.retained_bytes();
        let cpu_replay_available = prepared.cpu_replay_available();
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
                "retained bytes must be in 1..=1 GiB",
            ));
        }
        if requirements.requires_cpu_replay() && !cpu_replay_available {
            return Err(BridgeError::InvalidEditDetailOutput(
                "prepared detail source does not satisfy the required CPU replay capability",
            ));
        }
        Ok(Self {
            handle,
            dimensions: prepared_dimensions,
            retained_bytes,
            cpu_replay_available,
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

    /// Returns the actual immutable packed-u16 or scene-linear-fp32 allocation retained by this
    /// session.
    #[must_use]
    pub const fn retained_bytes(&self) -> u64 {
        self.retained_bytes
    }

    /// Whether this retained source can execute a complete tile through the
    /// portable CPU adjustment and structural path.
    #[must_use]
    pub const fn cpu_replay_available(&self) -> bool {
        self.cpu_replay_available
    }

    /// Reports whether this prepared source satisfies a later plan's runtime
    /// requirements and is therefore safe to reuse from a session cache.
    #[must_use]
    pub const fn satisfies_requirements(&self, requirements: DetailSessionRequirements) -> bool {
        !requirements.requires_cpu_replay() || self.cpu_replay_available
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
        let execution = detail_execution_receipt(
            rendered.execution_backend,
            rendered.execution_backend_version,
            rendered.source_cache_hit,
            rendered.fell_back,
            rendered.diagnostic,
        )?;
        Ok(RenderedDetailTile {
            rect,
            full_dimensions,
            row_stride_bytes: rendered.row_stride_bytes,
            bytes: rendered.bytes,
            execution,
        })
    }

    /// Executes the same dependency-ordered plan through the CPU high-bit
    /// display boundary and returns packed RGB16 without scaling or source I/O.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for an invalid plan or rectangle,
    /// [`BridgeError::Decoder`] for authoritative kernel failures, or
    /// [`BridgeError::InvalidEditDetailOutput`] if bridge output violates its contract.
    pub fn render_plan_tile16(
        &self,
        plan: &AdjustmentRenderPlan,
        request: DetailTileRequest,
    ) -> Result<RenderedDetailTile16, BridgeError> {
        plan.validate()?;
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        request.validate(output_dimensions)?;
        if !self.cpu_replay_available {
            return Err(BridgeError::InvalidEditRequest(
                "RGB16 detail output requires a CPU-replayable source",
            ));
        }
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let rendered =
            handle.render_adjustment_plan_tile16(&ffi_detail_tile_request(plan, request))?;
        let rect = detail_tile_rect(rendered.rect);
        let full_dimensions = dimensions(&rendered.full_dimensions);
        if rect != request.rect || full_dimensions != output_dimensions {
            return Err(BridgeError::InvalidEditDetailOutput(
                "returned identity does not match the requested RGB16 tile and prepared source",
            ));
        }
        let expected_stride = rect
            .width
            .checked_mul(3)
            .and_then(|value| value.checked_mul(2))
            .ok_or(BridgeError::InvalidEditDetailOutput(
                "RGB16 row stride overflows",
            ))?;
        if rendered.row_stride_bytes != expected_stride {
            return Err(BridgeError::InvalidEditDetailOutput(
                "RGB16 row stride must equal width times six",
            ));
        }
        let expected_len = usize::try_from(rect.width)
            .ok()
            .and_then(|width| width.checked_mul(3))
            .and_then(|row_samples| {
                usize::try_from(rect.height)
                    .ok()
                    .and_then(|height| row_samples.checked_mul(height))
            })
            .ok_or(BridgeError::InvalidEditDetailOutput(
                "RGB16 sample length overflows addressable memory",
            ))?;
        if rendered.samples.len() != expected_len {
            return Err(BridgeError::InvalidEditDetailOutput(
                "RGB16 sample length must equal width times height times three",
            ));
        }
        let execution = detail_execution_receipt(
            rendered.execution_backend,
            rendered.execution_backend_version,
            rendered.source_cache_hit,
            rendered.fell_back,
            rendered.diagnostic,
        )?;
        if execution.backend != DetailTileRenderBackend::Cpu || execution.fell_back {
            return Err(BridgeError::InvalidEditDetailOutput(
                "RGB16 detail output must report the direct CPU export boundary",
            ));
        }
        Ok(RenderedDetailTile16 {
            rect,
            full_dimensions,
            row_stride_bytes: rendered.row_stride_bytes,
            samples: rendered.samples,
            execution,
        })
    }
}
