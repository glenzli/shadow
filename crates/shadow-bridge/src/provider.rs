//! Provider identities, supported source declarations, and RAW preflight negotiation.

use std::path::Path;

use super::{
    BridgeError,
    decoder::open_photo,
    ffi,
    raw_development::{
        RawDevelopmentCapabilities, RawDevelopmentPlan, RawDevelopmentPlanNegotiation,
        ffi_raw_development_plan, raw_development_capabilities, raw_development_plan_negotiation,
    },
};

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
