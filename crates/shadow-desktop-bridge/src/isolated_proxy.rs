//! Crash-isolated generated-preview and descriptor access for the desktop.
//!
//! The module tree is the ownership index:
//! - route identity binds source, helper, environment, protocol, and operation;
//! - persistent evidence records exact-route crash and timeout observations;
//! - native admission derives the conservative in-process circuit breaker;
//! - helper process owns bounded execution and descendant cleanup;
//! - reference proxy, metadata, and decoder snapshot own their complete
//!   protocol, cache, and orchestration pipelines.

mod decoder_snapshot;
mod helper_process;
mod helper_wire_fields;
mod metadata;
mod native_admission;
mod persistent_evidence;
mod reference_proxy;
mod route_identity;

// Keep return types nameable through the same facade as their entry points,
// even where current callers rely on type inference.
#[allow(unused_imports)]
pub(crate) use decoder_snapshot::IsolatedPhotoDecoderSnapshot;
pub(crate) use decoder_snapshot::snapshot_isolated_photo_decoder;
#[allow(unused_imports)]
pub(crate) use metadata::IsolatedPhotoMetadataSnapshot;
pub(crate) use metadata::snapshot_isolated_photo_metadata;
pub(crate) use native_admission::{
    NativeDecodeAdmission, native_decode_admission_after_isolated_stages,
};
#[allow(unused_imports)]
pub(crate) use persistent_evidence::IsolatedDecodeObservation;
pub(crate) use reference_proxy::{
    render_isolated_photo_reference_proxy, render_isolated_photo_reference_proxy_to_file,
};
pub(crate) use route_identity::{configured_helper_path, isolated_helper_implementation_identity};

#[cfg(test)]
mod descriptor_protocol_fixture;
#[cfg(test)]
mod route_fixture;
