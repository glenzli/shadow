//! Platform/provider adapters that expose honest availability.
//!
//! The current Apple Vision owner is a non-linking skeleton. It validates the
//! runtime boundary and returns an explicit unavailable terminal until a native
//! Vision bridge supplies real feature-print distances.

mod apple_vision;

pub use apple_vision::AppleVisionFeaturePrintProvider;
