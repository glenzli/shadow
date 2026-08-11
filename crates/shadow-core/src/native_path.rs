//! Compatibility facade for the shared native-path contract.
//!
//! New low-level consumers should depend on `shadow-native-path` directly.
//! `shadow-core` keeps these exports so its existing workflow API remains
//! source-compatible while callers migrate independently.

pub use shadow_native_path::{NativePathError, native_location, native_path_from_location};

pub(crate) use shadow_native_path::native_location as encode_location;
pub(crate) use shadow_native_path::native_path_from_location as decode_location;
