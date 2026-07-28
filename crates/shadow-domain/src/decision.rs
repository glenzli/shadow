use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::PhotoId;

const MAX_DECISION_EVENT_ID_LENGTH: usize = 256;
pub const MAX_PHOTO_RATING: u8 = 5;

/// The authoritative culling flag attached to one logical photo.
#[derive(Debug, Default, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PhotoFlag {
    #[default]
    Unflagged,
    Picked,
    Rejected,
}

impl PhotoFlag {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Unflagged => "unflagged",
            Self::Picked => "picked",
            Self::Rejected => "rejected",
        }
    }
}

/// Provenance of an authoritative photo decision.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PhotoDecisionOrigin {
    Human,
}

impl PhotoDecisionOrigin {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Human => "human",
        }
    }
}

/// Current authoritative decision for one photo.
///
/// Sequence zero is the implicit state before the first ledger event.
#[derive(Debug, Default, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct PhotoDecisionState {
    pub head_sequence: u64,
    pub flag: PhotoFlag,
    pub rating: u8,
}

impl PhotoDecisionState {
    /// Builds one valid current state.
    ///
    /// # Errors
    ///
    /// Returns [`PhotoDecisionValidationError`] for a rating above five or a
    /// non-default state paired with the implicit zero head.
    pub fn new(
        head_sequence: u64,
        flag: PhotoFlag,
        rating: u8,
    ) -> Result<Self, PhotoDecisionValidationError> {
        validate_rating("photo decision rating", rating)?;
        if head_sequence == 0 && (flag != PhotoFlag::Unflagged || rating != 0) {
            return Err(PhotoDecisionValidationError::NonDefaultInitialState);
        }
        Ok(Self {
            head_sequence,
            flag,
            rating,
        })
    }
}

/// Optimistic request to append one real human decision transition.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct NewPhotoDecisionEvent {
    pub event_id: String,
    pub photo_id: PhotoId,
    pub occurred_at_unix_ms: i64,
    pub origin: PhotoDecisionOrigin,
    pub expected_head_sequence: u64,
    pub before_flag: PhotoFlag,
    pub before_rating: u8,
    pub after_flag: PhotoFlag,
    pub after_rating: u8,
}

impl NewPhotoDecisionEvent {
    /// Validates the caller-supplied transition without assigning a sequence.
    ///
    /// # Errors
    ///
    /// Returns [`PhotoDecisionValidationError`] for invalid identity, rating,
    /// initial-state, or no-op transition data.
    pub fn validate(&self) -> Result<(), PhotoDecisionValidationError> {
        validate_event_id(&self.event_id)?;
        validate_rating("photo decision before rating", self.before_rating)?;
        validate_rating("photo decision after rating", self.after_rating)?;
        validate_initial_before(
            self.expected_head_sequence,
            self.before_flag,
            self.before_rating,
        )?;
        validate_real_change(
            self.before_flag,
            self.before_rating,
            self.after_flag,
            self.after_rating,
        )
    }

    /// Returns the state against which Catalog must compare-and-swap.
    ///
    /// # Errors
    ///
    /// Returns [`PhotoDecisionValidationError`] when this request is invalid.
    pub fn before_state(&self) -> Result<PhotoDecisionState, PhotoDecisionValidationError> {
        self.validate()?;
        PhotoDecisionState::new(
            self.expected_head_sequence,
            self.before_flag,
            self.before_rating,
        )
    }

    /// Assigns Catalog's global ledger sequence and creates the immutable fact.
    ///
    /// # Errors
    ///
    /// Returns [`PhotoDecisionValidationError`] when the request is invalid or
    /// the assigned sequence does not strictly follow its expected head.
    pub fn with_sequence(
        self,
        sequence: u64,
    ) -> Result<PhotoDecisionEvent, PhotoDecisionValidationError> {
        self.validate()?;
        if sequence == 0 {
            return Err(PhotoDecisionValidationError::ZeroSequence);
        }
        if sequence <= self.expected_head_sequence {
            return Err(PhotoDecisionValidationError::NonMonotonicSequence {
                sequence,
                before_head_sequence: self.expected_head_sequence,
            });
        }
        Ok(PhotoDecisionEvent {
            event_id: self.event_id,
            sequence,
            photo_id: self.photo_id,
            occurred_at_unix_ms: self.occurred_at_unix_ms,
            origin: self.origin,
            before_head_sequence: self.expected_head_sequence,
            before_flag: self.before_flag,
            before_rating: self.before_rating,
            after_flag: self.after_flag,
            after_rating: self.after_rating,
        })
    }
}

/// One immutable transition in the authoritative photo-decision ledger.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct PhotoDecisionEvent {
    pub event_id: String,
    pub sequence: u64,
    pub photo_id: PhotoId,
    pub occurred_at_unix_ms: i64,
    pub origin: PhotoDecisionOrigin,
    pub before_head_sequence: u64,
    pub before_flag: PhotoFlag,
    pub before_rating: u8,
    pub after_flag: PhotoFlag,
    pub after_rating: u8,
}

impl PhotoDecisionEvent {
    /// Validates one fully assigned immutable decision fact.
    ///
    /// # Errors
    ///
    /// Returns [`PhotoDecisionValidationError`] for invalid identity, state,
    /// rating, no-op, or sequence relationships.
    pub fn validate(&self) -> Result<(), PhotoDecisionValidationError> {
        validate_event_id(&self.event_id)?;
        if self.sequence == 0 {
            return Err(PhotoDecisionValidationError::ZeroSequence);
        }
        if self.sequence <= self.before_head_sequence {
            return Err(PhotoDecisionValidationError::NonMonotonicSequence {
                sequence: self.sequence,
                before_head_sequence: self.before_head_sequence,
            });
        }
        validate_rating("photo decision before rating", self.before_rating)?;
        validate_rating("photo decision after rating", self.after_rating)?;
        validate_initial_before(
            self.before_head_sequence,
            self.before_flag,
            self.before_rating,
        )?;
        validate_real_change(
            self.before_flag,
            self.before_rating,
            self.after_flag,
            self.after_rating,
        )
    }

    /// Reconstructs the decision state immediately before this event.
    ///
    /// # Errors
    ///
    /// Returns [`PhotoDecisionValidationError`] when the event is invalid.
    pub fn before_state(&self) -> Result<PhotoDecisionState, PhotoDecisionValidationError> {
        self.validate()?;
        PhotoDecisionState::new(
            self.before_head_sequence,
            self.before_flag,
            self.before_rating,
        )
    }

    /// Reconstructs the current state produced by this event.
    ///
    /// # Errors
    ///
    /// Returns [`PhotoDecisionValidationError`] when the event is invalid.
    pub fn after_state(&self) -> Result<PhotoDecisionState, PhotoDecisionValidationError> {
        self.validate()?;
        PhotoDecisionState::new(self.sequence, self.after_flag, self.after_rating)
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Error)]
pub enum PhotoDecisionValidationError {
    #[error("photo decision event id must contain 1 through 256 bytes")]
    InvalidEventId,
    #[error("{field} {rating} is outside 0 through {maximum}")]
    InvalidRating {
        field: &'static str,
        rating: u8,
        maximum: u8,
    },
    #[error("photo decision sequence must be greater than zero")]
    ZeroSequence,
    #[error(
        "photo decision sequence {sequence} must follow before-head sequence {before_head_sequence}"
    )]
    NonMonotonicSequence {
        sequence: u64,
        before_head_sequence: u64,
    },
    #[error("head sequence zero must use the default unflagged, zero-rating state")]
    NonDefaultInitialState,
    #[error("photo decision transition must change flag or rating")]
    NoStateChange,
}

fn validate_event_id(event_id: &str) -> Result<(), PhotoDecisionValidationError> {
    if event_id.trim().is_empty() || event_id.len() > MAX_DECISION_EVENT_ID_LENGTH {
        Err(PhotoDecisionValidationError::InvalidEventId)
    } else {
        Ok(())
    }
}

fn validate_rating(field: &'static str, rating: u8) -> Result<(), PhotoDecisionValidationError> {
    if rating > MAX_PHOTO_RATING {
        Err(PhotoDecisionValidationError::InvalidRating {
            field,
            rating,
            maximum: MAX_PHOTO_RATING,
        })
    } else {
        Ok(())
    }
}

fn validate_initial_before(
    before_head_sequence: u64,
    before_flag: PhotoFlag,
    before_rating: u8,
) -> Result<(), PhotoDecisionValidationError> {
    if before_head_sequence == 0 && (before_flag != PhotoFlag::Unflagged || before_rating != 0) {
        Err(PhotoDecisionValidationError::NonDefaultInitialState)
    } else {
        Ok(())
    }
}

fn validate_real_change(
    before_flag: PhotoFlag,
    before_rating: u8,
    after_flag: PhotoFlag,
    after_rating: u8,
) -> Result<(), PhotoDecisionValidationError> {
    if before_flag == after_flag && before_rating == after_rating {
        Err(PhotoDecisionValidationError::NoStateChange)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests;
