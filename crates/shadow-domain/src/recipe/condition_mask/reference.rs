//! Normative, portable condition-mask numerical contract.
//!
//! Follow `scalar` for pixel predicates and fuzzy expression operators, and
//! `local_detail` for full-render radius conversion, neighborhood footprint,
//! and residual normalization. These owners are deliberately unoptimized:
//! CPU and accelerator implementations may reorganize execution, but their
//! output must match the reference vectors within their declared tolerance.
//!
//! Soft expression operators are exact fuzzy-set operations: `all` is the
//! minimum child coverage, `any` is the maximum, and `not(x)` is `1 - x`.
//! Lightness, chroma, and local-detail ranges use the quintic
//! `q(t) = x³ * (x * (6x - 15) + 10)`, where `x = clamp(t, 0, 1)`. For
//! positive softness `s`, coverage is
//! `min(q((v - (lower - s)) / s), 1 - q((v - upper) / s))`; zero softness is
//! an inclusive hard range.
//!
//! Hue preserves the existing cubic `smoothstep(a, b, v)` relative-chroma
//! confidence with `a = 0.002`, `b = 0.02`, and
//! `v = C / max(1e-6, abs(L))`. Hue distance is circular; `softness` feathers
//! inward over `half_width * softness`. A nonzero absolute normalized-chroma
//! minimum uses its separate feather:
//! `smoothstep(max(0, minimum - feather), minimum, clamp(C / 0.4, 0, 1))`.
//! A zero minimum returns an exact gate value of one.

mod local_detail;
mod scalar;

pub use local_detail::{
    ConditionMaskReferenceError, LocalDetailFullRenderScale, local_detail_reference_response,
};
pub use scalar::{
    CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_LOWER, CONDITION_HUE_RELATIVE_CHROMA_CONFIDENCE_UPPER,
    ConditionMaskScalarSample,
};
