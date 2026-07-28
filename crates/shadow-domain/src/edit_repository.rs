//! Content-addressed edit repository contracts.
//!
//! This module is the stable public index for six cohesive owners:
//!
//! - `content_id` defines domain-separated object and commit identities;
//! - `error` defines stable validation and reconstruction failures;
//! - `repository_object` owns canonical object bytes and individual edge records;
//! - `library_state` owns the typed Library root and sorted entity maps;
//! - `object_pack` binds typed payloads to their complete verified edge index;
//! - `history` owns immutable repository commits and movable-ref expectations.
//!
//! The owners remain private so the crate-root API and serialized contracts do
//! not depend on the internal source layout.

mod content_id;
mod error;
mod history;
mod library_state;
mod object_pack;
mod repository_object;

pub use content_id::{EditCommitId, EditObjectId};
pub use error::EditRepositoryError;
pub use history::{
    EditRepositoryCommit, EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation,
    EditRepositoryRefKind,
};
pub use library_state::{EditEntityChangeV1, EditEntityEntryV1, EditEntityMapV1, LibraryRootV1};
pub use object_pack::EditObjectPack;
pub use repository_object::{EditObject, EditObjectEdge, EditObjectKind};
