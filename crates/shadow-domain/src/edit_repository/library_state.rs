//! Typed Library root and sorted entity-map projections.

use serde::{Deserialize, Serialize};

use super::{content_id::EditObjectId, error::EditRepositoryError};

const MAX_ENTITY_KEY_BYTES: usize = 512;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct LibraryRootV1 {
    pub photo_recipes: Option<EditObjectId>,
    pub shared_grade_heads: Option<EditObjectId>,
    pub masks: Option<EditObjectId>,
    pub styles: Option<EditObjectId>,
    pub output_states: Option<EditObjectId>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct EditEntityEntryV1 {
    pub key: String,
    pub value: EditObjectId,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct EditEntityMapV1 {
    entries: Vec<EditEntityEntryV1>,
}

impl EditEntityMapV1 {
    /// Creates a deterministic map, sorting entries by their stable key.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for invalid or duplicate keys.
    pub fn new(mut entries: Vec<EditEntityEntryV1>) -> Result<Self, EditRepositoryError> {
        entries.sort_by(|left, right| left.key.cmp(&right.key));
        let map = Self { entries };
        map.validate()?;
        Ok(map)
    }

    #[must_use]
    pub fn entries(&self) -> &[EditEntityEntryV1] {
        &self.entries
    }

    #[must_use]
    pub fn get(&self, key: &str) -> Option<EditObjectId> {
        self.entries
            .binary_search_by(|entry| entry.key.as_str().cmp(key))
            .ok()
            .map(|index| self.entries[index].value)
    }

    /// Returns a new map with one key inserted or atomically replaced.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] when the supplied key is invalid.
    pub fn with_entry(
        &self,
        key: impl Into<String>,
        value: EditObjectId,
    ) -> Result<Self, EditRepositoryError> {
        let key = key.into();
        let mut entries = self.entries.clone();
        match entries.binary_search_by(|entry| entry.key.cmp(&key)) {
            Ok(index) => entries[index].value = value,
            Err(index) => entries.insert(index, EditEntityEntryV1 { key, value }),
        }
        Self::new(entries)
    }

    #[must_use]
    pub fn without_entry(&self, key: &str) -> Self {
        let mut entries = self.entries.clone();
        if let Ok(index) = entries.binary_search_by(|entry| entry.key.as_str().cmp(key)) {
            entries.remove(index);
        }
        Self { entries }
    }

    #[must_use]
    pub fn diff(&self, after: &Self) -> Vec<EditEntityChangeV1> {
        let mut changes = Vec::new();
        let (mut before_index, mut after_index) = (0, 0);
        while before_index < self.entries.len() || after_index < after.entries.len() {
            match (
                self.entries.get(before_index),
                after.entries.get(after_index),
            ) {
                (Some(before), Some(next)) if before.key == next.key => {
                    if before.value != next.value {
                        changes.push(EditEntityChangeV1 {
                            key: before.key.clone(),
                            before: Some(before.value),
                            after: Some(next.value),
                        });
                    }
                    before_index += 1;
                    after_index += 1;
                }
                (Some(before), Some(next)) if before.key < next.key => {
                    changes.push(EditEntityChangeV1 {
                        key: before.key.clone(),
                        before: Some(before.value),
                        after: None,
                    });
                    before_index += 1;
                }
                (Some(_) | None, Some(next)) => {
                    changes.push(EditEntityChangeV1 {
                        key: next.key.clone(),
                        before: None,
                        after: Some(next.value),
                    });
                    after_index += 1;
                }
                (Some(before), None) => {
                    changes.push(EditEntityChangeV1 {
                        key: before.key.clone(),
                        before: Some(before.value),
                        after: None,
                    });
                    before_index += 1;
                }
                (None, None) => break,
            }
        }
        changes
    }

    pub(super) fn validate(&self) -> Result<(), EditRepositoryError> {
        let mut previous: Option<&str> = None;
        for entry in &self.entries {
            if entry.key.is_empty()
                || entry.key.len() > MAX_ENTITY_KEY_BYTES
                || entry.key.chars().any(char::is_control)
            {
                return Err(EditRepositoryError::InvalidEntityKey(entry.key.clone()));
            }
            if previous.is_some_and(|value| value >= entry.key.as_str()) {
                return Err(EditRepositoryError::DuplicateOrUnsortedEntityKey(
                    entry.key.clone(),
                ));
            }
            previous = Some(&entry.key);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditEntityChangeV1 {
    pub key: String,
    pub before: Option<EditObjectId>,
    pub after: Option<EditObjectId>,
}

#[cfg(test)]
mod tests;
