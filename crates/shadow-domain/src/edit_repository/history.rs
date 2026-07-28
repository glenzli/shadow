//! Immutable repository commits and movable-reference expectations.

use std::{collections::BTreeSet, str::FromStr};

use serde::{Deserialize, Serialize};

use super::{
    content_id::{EditCommitId, EditObjectId, commit_id},
    error::EditRepositoryError,
};

const MAX_COMMIT_MESSAGE_BYTES: usize = 4 * 1024;
const MAX_COMMIT_PARENTS: usize = 8;

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct EditRepositoryCommitPayloadV1 {
    pub root: EditObjectId,
    pub parents: Vec<EditCommitId>,
    pub message: Option<String>,
    pub created_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditRepositoryCommit {
    id: EditCommitId,
    payload: EditRepositoryCommitPayloadV1,
    canonical_json: Vec<u8>,
}

impl EditRepositoryCommit {
    /// Creates one immutable Library-wide commit from a validated payload.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for duplicate/excessive parents, an
    /// invalid message, or JSON serialization failure.
    pub fn new(payload: EditRepositoryCommitPayloadV1) -> Result<Self, EditRepositoryError> {
        validate_commit_payload(&payload)?;
        let canonical_json = serde_json::to_vec(&payload)?;
        let id = commit_id(1, &canonical_json);
        Ok(Self {
            id,
            payload,
            canonical_json,
        })
    }

    /// Reconstructs a commit only when its canonical bytes match its identifier.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for malformed/non-canonical JSON, an
    /// invalid payload, or a digest mismatch.
    pub fn from_stored_parts(
        id: EditCommitId,
        canonical_json: Vec<u8>,
    ) -> Result<Self, EditRepositoryError> {
        let payload: EditRepositoryCommitPayloadV1 = serde_json::from_slice(&canonical_json)?;
        validate_commit_payload(&payload)?;
        if serde_json::to_vec(&payload)? != canonical_json {
            return Err(EditRepositoryError::NonCanonicalCommitPayload);
        }
        if commit_id(1, &canonical_json) != id {
            return Err(EditRepositoryError::CommitDigestMismatch { id });
        }
        Ok(Self {
            id,
            payload,
            canonical_json,
        })
    }

    #[must_use]
    pub const fn id(&self) -> EditCommitId {
        self.id
    }

    #[must_use]
    pub const fn payload(&self) -> &EditRepositoryCommitPayloadV1 {
        &self.payload
    }

    #[must_use]
    pub fn canonical_json(&self) -> &[u8] {
        &self.canonical_json
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum EditRepositoryRefKind {
    Branch,
    NamedVersion,
    Tag,
}

impl EditRepositoryRefKind {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Branch => "branch",
            Self::NamedVersion => "named_version",
            Self::Tag => "tag",
        }
    }
}

impl FromStr for EditRepositoryRefKind {
    type Err = EditRepositoryError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "branch" => Ok(Self::Branch),
            "named_version" => Ok(Self::NamedVersion),
            "tag" => Ok(Self::Tag),
            _ => Err(EditRepositoryError::UnknownRefKind(value.to_owned())),
        }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum EditRepositoryRefExpectation {
    Missing,
    At(EditCommitId),
}

fn validate_commit_payload(
    payload: &EditRepositoryCommitPayloadV1,
) -> Result<(), EditRepositoryError> {
    if payload.parents.len() > MAX_COMMIT_PARENTS {
        return Err(EditRepositoryError::TooManyCommitParents);
    }
    let unique: BTreeSet<_> = payload.parents.iter().copied().collect();
    if unique.len() != payload.parents.len() {
        return Err(EditRepositoryError::DuplicateCommitParent);
    }
    if payload.message.as_ref().is_some_and(|message| {
        message.is_empty()
            || message.len() > MAX_COMMIT_MESSAGE_BYTES
            || message.chars().any(|value| value == '\0')
    }) {
        return Err(EditRepositoryError::InvalidCommitMessage);
    }
    Ok(())
}

#[cfg(test)]
mod tests;
