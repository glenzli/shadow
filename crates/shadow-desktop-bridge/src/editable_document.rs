//! Self-contained independent-editor documents. The public ZIP envelope owns
//! byte identity and portable source/render locations; `archive` owns verified
//! publication/reopening and `history` owns the durable gesture graph. Neither
//! owner reads the daily Library or transports credentials/preferences.

mod archive;
mod history;
mod resources;

pub(crate) use archive::{pack_editable_document, unpack_editable_document};

#[cfg(test)]
mod tests;
