//! Interactive edit-preview lifecycle index.
//!
//! Follow [`service`] for the complete cancellable transaction,
//! [`warm_session_cache`] for decoded-source reuse, [`recipe_preview_store`]
//! for post-terminal durable publication, [`owned_response`] for the stable
//! cross-language payload lifetime, and [`response`] for descriptor projection.

mod owned_response;
mod recipe_preview_store;
mod response;
mod service;
mod warm_session_cache;

pub(crate) use owned_response::OwnedEditedPreview;
pub(crate) use recipe_preview_store::{RecipePreviewStoreJob, defer_recipe_preview_store};
pub(crate) use response::{cancelled_edited_preview, completed_edited_preview};
pub(crate) use service::EditPreviewPolicy;
pub(super) use warm_session_cache::{WarmEditPreviewSessionCache, WarmEditPreviewSourceRequest};

#[cfg(test)]
mod tests;
