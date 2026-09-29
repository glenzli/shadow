//! Recipe v1 desktop adapter index.
//!
//! Follow the responsibility-named children for editable drafts, Qt FFI
//! translation, validation, stable identity, snapshot serialization, canonical
//! persisted graph layout, and executable render-plan compilation.
//! `shadow-domain` may persist bounded condition expressions before this
//! adapter can execute them; `snapshot_decode` and `compiler` own the explicit
//! fail-closed capability gates instead of flattening or dropping predicates.
//! Managed rasters cross Qt as opaque kind-six references plus reversible
//! expansion, feather, and invert controls; their exact immutable bytes are
//! restored from the explicit base Recipe and resolved through verified
//! application storage immediately before render-plan compilation.
//! `foundation_development` compiles white balance, independent AI-source
//! selection, Foundation bypass, and optics from the same immutable snapshot
//! as the Grade plan; every render intent consumes that one source contract,
//! and lossy RGB compatibility routes reject manual RAW white balance.
//! `ffi_adapter::liquify` owns strict decoding of the bounded flat Liquify DTO.

use shadow_bridge::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION as OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_REVISION,
    SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION as SELECTIVE_TONE_PARAMETER_SCHEMA_REVISION,
};
use shadow_domain::operation::{
    CPU_REFERENCE_IMPLEMENTATION_REVISION, CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION, SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION,
};

mod compiler;
mod draft;
mod ffi_adapter;
mod foundation_development;
mod identity;
mod managed_raster_resolution;
mod paint;
mod render_request;
mod rgb_tone_curves;
mod snapshot_decode;
mod snapshot_encode;
mod snapshot_layout;
mod validation;

pub(crate) use compiler::*;
pub(crate) use draft::*;
pub(crate) use ffi_adapter::*;
pub(crate) use foundation_development::*;
pub(crate) use identity::*;
pub(crate) use render_request::*;
pub(crate) use snapshot_decode::*;
pub(crate) use snapshot_encode::*;
pub(crate) use snapshot_layout::*;
pub(crate) use validation::*;

const _: () = assert!(
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION == ADJUSTMENT_PARAMETER_SCHEMA_VERSION
        && CPU_REFERENCE_IMPLEMENTATION_REVISION == ADJUSTMENT_IMPLEMENTATION_VERSION
        && OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION
            == OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_REVISION
        && SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION == SELECTIVE_TONE_PARAMETER_SCHEMA_REVISION
);

pub(crate) const MAX_GRADE_NODES: usize = 16;
pub(crate) const CONTRAST_PIVOT: f64 = 0.18;
