//! Recipe v1 desktop adapter index.
//!
//! Follow the responsibility-named children for editable drafts, Qt FFI
//! translation, validation, stable identity, snapshot serialization, canonical
//! persisted graph layout, and executable render-plan compilation.
//! `shadow-domain` may persist bounded condition expressions before this
//! adapter can execute them; `snapshot_decode` and `compiler` own the explicit
//! fail-closed capability gates instead of flattening or dropping predicates.

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
mod identity;
mod render_request;
mod snapshot_decode;
mod snapshot_encode;
mod snapshot_layout;
mod validation;

pub(crate) use compiler::*;
pub(crate) use draft::*;
pub(crate) use ffi_adapter::*;
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
