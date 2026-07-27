//! Immutable Recipe commits, movable refs, named versions, and history validation.

use std::collections::{BTreeSet, HashMap, HashSet};

use serde::{Deserialize, Serialize};

use crate::{BranchId, RecipeCommitId, RecipeId, VersionId};

use super::value::{
    BranchName, MAX_COMMIT_MESSAGE_BYTES, VersionName, display_name_character, validate_text,
};
use super::{RecipeSnapshot, RecipeValidationError};

/// An immutable recipe state. Moving a branch creates a new `RecipeBranch`
/// value; an existing commit never changes its snapshot or parents.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecipeCommit {
    id: RecipeCommitId,
    recipe_id: RecipeId,
    parents: Vec<RecipeCommitId>,
    snapshot: RecipeSnapshot,
    message: Option<String>,
    created_at_ms: i64,
}

impl RecipeCommit {
    /// Creates an immutable recipe commit with zero, one, or two parents.
    ///
    /// # Errors
    ///
    /// Returns an error for an invalid snapshot, unresolved shared revisions,
    /// invalid parent sets, or an invalid commit message.
    pub fn new(
        id: RecipeCommitId,
        recipe_id: RecipeId,
        parents: Vec<RecipeCommitId>,
        snapshot: RecipeSnapshot,
        message: Option<String>,
        created_at_ms: i64,
    ) -> Result<Self, RecipeValidationError> {
        let commit = Self {
            id,
            recipe_id,
            parents,
            snapshot,
            message,
            created_at_ms,
        };
        commit.validate()?;
        Ok(commit)
    }

    pub const fn id(&self) -> RecipeCommitId {
        self.id
    }

    pub const fn recipe_id(&self) -> RecipeId {
        self.recipe_id
    }

    pub fn parents(&self) -> &[RecipeCommitId] {
        &self.parents
    }

    pub fn snapshot(&self) -> &RecipeSnapshot {
        &self.snapshot
    }

    pub fn message(&self) -> Option<&str> {
        self.message.as_deref()
    }

    pub const fn created_at_ms(&self) -> i64 {
        self.created_at_ms
    }

    /// Checks a commit read from a persistence or interchange boundary.
    ///
    /// # Errors
    ///
    /// Returns the first snapshot, parent, or message invariant violation.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        self.snapshot.validate_for_commit()?;
        if self.parents.len() > 2 {
            return Err(RecipeValidationError::TooManyCommitParents(
                self.parents.len(),
            ));
        }
        let mut parent_ids = HashSet::with_capacity(self.parents.len());
        for parent in &self.parents {
            if *parent == self.id {
                return Err(RecipeValidationError::SelfParentCommit(self.id));
            }
            if !parent_ids.insert(*parent) {
                return Err(RecipeValidationError::DuplicateCommitParent(*parent));
            }
        }
        if let Some(message) = &self.message {
            validate_text(
                message,
                MAX_COMMIT_MESSAGE_BYTES,
                "commit message",
                display_name_character,
            )?;
        }
        Ok(())
    }
}

/// A movable branch ref. Updating it means replacing this value, not mutating
/// a commit.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct RecipeBranch {
    id: BranchId,
    recipe_id: RecipeId,
    name: BranchName,
    target: RecipeCommitId,
}

impl RecipeBranch {
    pub const fn new(
        id: BranchId,
        recipe_id: RecipeId,
        name: BranchName,
        target: RecipeCommitId,
    ) -> Self {
        Self {
            id,
            recipe_id,
            name,
            target,
        }
    }

    pub const fn id(&self) -> BranchId {
        self.id
    }

    pub const fn recipe_id(&self) -> RecipeId {
        self.recipe_id
    }

    pub fn name(&self) -> &BranchName {
        &self.name
    }

    pub const fn target(&self) -> RecipeCommitId {
        self.target
    }
}

/// A permanent, user-visible name for one exact commit.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct NamedVersion {
    id: VersionId,
    recipe_id: RecipeId,
    name: VersionName,
    target: RecipeCommitId,
    created_at_ms: i64,
}

impl NamedVersion {
    pub const fn new(
        id: VersionId,
        recipe_id: RecipeId,
        name: VersionName,
        target: RecipeCommitId,
        created_at_ms: i64,
    ) -> Self {
        Self {
            id,
            recipe_id,
            name,
            target,
            created_at_ms,
        }
    }

    pub const fn id(&self) -> VersionId {
        self.id
    }

    pub const fn recipe_id(&self) -> RecipeId {
        self.recipe_id
    }

    pub fn name(&self) -> &VersionName {
        &self.name
    }

    pub const fn target(&self) -> RecipeCommitId {
        self.target
    }

    pub const fn created_at_ms(&self) -> i64 {
        self.created_at_ms
    }
}

/// A self-contained history snapshot suitable for validation, export, or
/// cross-catalog transfer. Catalogs may store its records in normalized tables.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecipeHistory {
    recipe_id: RecipeId,
    commits: Vec<RecipeCommit>,
    branches: Vec<RecipeBranch>,
    versions: Vec<NamedVersion>,
}

impl RecipeHistory {
    /// Creates and validates one recipe's commit graph and human-facing refs.
    ///
    /// # Errors
    ///
    /// Returns an error for malformed commits, missing parents or targets,
    /// cycles, multiple roots, duplicate refs, or mixed recipe identities.
    pub fn new(
        recipe_id: RecipeId,
        commits: Vec<RecipeCommit>,
        branches: Vec<RecipeBranch>,
        versions: Vec<NamedVersion>,
    ) -> Result<Self, RecipeValidationError> {
        let history = Self {
            recipe_id,
            commits,
            branches,
            versions,
        };
        history.validate()?;
        Ok(history)
    }

    pub const fn recipe_id(&self) -> RecipeId {
        self.recipe_id
    }

    pub fn commits(&self) -> &[RecipeCommit] {
        &self.commits
    }

    pub fn branches(&self) -> &[RecipeBranch] {
        &self.branches
    }

    pub fn versions(&self) -> &[NamedVersion] {
        &self.versions
    }

    /// Validates a complete history read from persistence or interchange.
    ///
    /// # Errors
    ///
    /// Returns the first commit-DAG or reference invariant violation found.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        if self.commits.is_empty() {
            return Err(RecipeValidationError::EmptyHistory);
        }

        let mut commits = HashMap::with_capacity(self.commits.len());
        for commit in &self.commits {
            commit.validate()?;
            if commit.recipe_id != self.recipe_id {
                return Err(RecipeValidationError::RecipeIdMismatch {
                    expected: self.recipe_id,
                    actual: commit.recipe_id,
                });
            }
            if commits.insert(commit.id, commit).is_some() {
                return Err(RecipeValidationError::DuplicateCommit(commit.id));
            }
        }

        let roots = self
            .commits
            .iter()
            .filter(|commit| commit.parents.is_empty())
            .count();
        if roots != 1 {
            return Err(RecipeValidationError::InvalidRootCommitCount(roots));
        }
        for commit in &self.commits {
            for parent in &commit.parents {
                if !commits.contains_key(parent) {
                    return Err(RecipeValidationError::UnknownCommitParent {
                        commit_id: commit.id,
                        parent_id: *parent,
                    });
                }
            }
        }
        validate_commit_acyclic(&self.commits)?;

        let mut branch_ids = HashSet::with_capacity(self.branches.len());
        let mut branch_names = BTreeSet::new();
        for branch in &self.branches {
            if branch.recipe_id != self.recipe_id {
                return Err(RecipeValidationError::RecipeIdMismatch {
                    expected: self.recipe_id,
                    actual: branch.recipe_id,
                });
            }
            if !branch_ids.insert(branch.id) {
                return Err(RecipeValidationError::DuplicateBranch(branch.id));
            }
            if !branch_names.insert(branch.name.clone()) {
                return Err(RecipeValidationError::DuplicateBranchName(
                    branch.name.clone(),
                ));
            }
            if !commits.contains_key(&branch.target) {
                return Err(RecipeValidationError::UnknownRefTarget(branch.target));
            }
        }

        let mut version_ids = HashSet::with_capacity(self.versions.len());
        let mut version_names = BTreeSet::new();
        for version in &self.versions {
            if version.recipe_id != self.recipe_id {
                return Err(RecipeValidationError::RecipeIdMismatch {
                    expected: self.recipe_id,
                    actual: version.recipe_id,
                });
            }
            if !version_ids.insert(version.id) {
                return Err(RecipeValidationError::DuplicateVersion(version.id));
            }
            if !version_names.insert(version.name.clone()) {
                return Err(RecipeValidationError::DuplicateVersionName(
                    version.name.clone(),
                ));
            }
            if !commits.contains_key(&version.target) {
                return Err(RecipeValidationError::UnknownRefTarget(version.target));
            }
        }
        Ok(())
    }
}

fn validate_commit_acyclic(commits: &[RecipeCommit]) -> Result<(), RecipeValidationError> {
    #[derive(Copy, Clone, Eq, PartialEq)]
    enum Visit {
        Active,
        Complete,
    }

    fn visit(
        commit_id: RecipeCommitId,
        parents: &HashMap<RecipeCommitId, &[RecipeCommitId]>,
        visits: &mut HashMap<RecipeCommitId, Visit>,
    ) -> Result<(), RecipeValidationError> {
        match visits.get(&commit_id) {
            Some(Visit::Active) => return Err(RecipeValidationError::CommitCycle(commit_id)),
            Some(Visit::Complete) => return Ok(()),
            None => {}
        }
        visits.insert(commit_id, Visit::Active);
        if let Some(parent_ids) = parents.get(&commit_id) {
            for parent in *parent_ids {
                visit(*parent, parents, visits)?;
            }
        }
        visits.insert(commit_id, Visit::Complete);
        Ok(())
    }

    let parents = commits
        .iter()
        .map(|commit| (commit.id, commit.parents.as_slice()))
        .collect::<HashMap<_, _>>();
    let mut visits = HashMap::with_capacity(commits.len());
    for commit in commits {
        visit(commit.id, &parents, &mut visits)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests;
