//! Interactive edit-preview lifecycle index.
//!
//! Follow [`service`] for the complete cancellable transaction,
//! [`warm_session_cache`] for decoded-source reuse, [`recipe_preview_store`]
//! for post-terminal durable publication, and [`response`] for the final FFI
//! projection.

mod recipe_preview_store;
mod response;
mod service;
mod warm_session_cache;

pub(crate) use recipe_preview_store::{RecipePreviewStoreRequest, store_recipe_preview};
pub(crate) use response::{cancelled_edited_preview, completed_edited_preview};
pub(crate) use service::EditPreviewPolicy;
pub(super) use warm_session_cache::WarmEditPreviewSessionCache;

#[cfg(test)]
mod tests;
