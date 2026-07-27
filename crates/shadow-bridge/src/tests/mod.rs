//! Responsibility-indexed tests for the coarse Rust/C++ image boundary.
//!
//! Keep private invariants beside one implementation when they are small. This tree owns
//! cross-boundary contracts that require access to the crate-private CXX representation.

use std::path::{Path, PathBuf};

use shadow_domain::{ImageDimensions, PreviewCodec};

use super::*;
use super::{
    adjustment::validate_render_operation,
    decoder::open_libraw,
    preview_analysis::{edit_preview_execution_receipt, validate_edit_preview_analysis},
    raw_development::{raw_development_receipt, raw_pipeline_receipt},
    render_wire::ffi_render_node,
};

mod adjustment_plan;
mod detail_contract;
mod display_luma;
mod preview_contract;
mod raw_development;
mod real_sources;
