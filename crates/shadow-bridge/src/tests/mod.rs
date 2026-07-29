//! Responsibility-indexed tests for the coarse Rust/C++ image boundary.
//!
//! Keep private invariants beside one implementation when they are small. This tree owns
//! cross-boundary contracts that require access to the crate-private CXX representation.

mod adjustment_plan;
mod detail_contract;
mod display_luma;
mod native_build_input_contract;
mod oklab_color_warper_contract;
mod perceptual_color_contract;
mod preview_contract;
mod raw_development;
mod real_sources;
