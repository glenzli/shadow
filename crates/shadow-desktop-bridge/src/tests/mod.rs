//! Responsibility-indexed unit tests for the desktop bridge facade.
//!
//! Keep private facade invariants in this crate-local tree. Put tests beside a
//! production service when that service owns the behavior, and reserve this
//! index for contracts that genuinely cross desktop bridge responsibilities.

mod adjustment_contract;
mod edit_sessions;
mod facade;
mod fixtures;
mod history_contract;
mod library;
mod oklab_color_warper_contract;
mod perceptual_color_contract;
mod preview_and_detail;
mod raw_fixtures;
mod raw_inspection;
mod recipe_compiler;
mod recipe_identity;
mod review;
