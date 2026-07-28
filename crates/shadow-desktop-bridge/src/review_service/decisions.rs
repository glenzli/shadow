//! Durable human Review decision mutations and ABI projection.

use anyhow::{Context, Result as AnyResult, bail};
use shadow_domain::{
    MAX_PHOTO_RATING, NewPhotoDecisionEvent, PhotoDecisionEvent, PhotoDecisionOrigin,
    PhotoDecisionState, PhotoFlag, PhotoId,
};
use uuid::Uuid;

use crate::{ffi, wall_clock::current_time_ms};

use super::ReviewService;

impl ReviewService {
    pub(crate) fn photo_decision_state(
        &self,
        photo_id: &str,
    ) -> AnyResult<ffi::FfiPhotoDecisionState> {
        let photo_id: PhotoId = photo_id
            .parse()
            .with_context(|| format!("parse Review decision photo id {photo_id}"))?;
        Ok(ffi_photo_decision_state(
            photo_id,
            self.catalog.photo_decision_state(photo_id)?,
        ))
    }

    pub(crate) fn set_photo_decision(
        &self,
        photo_id: &str,
        expected_head_sequence: u64,
        flag: ffi::FfiDecisionFlag,
        rating: u8,
    ) -> AnyResult<ffi::FfiReviewDecisionMutationReceipt> {
        let photo_id: PhotoId = photo_id
            .parse()
            .with_context(|| format!("parse Review decision photo id {photo_id}"))?;
        if rating > MAX_PHOTO_RATING {
            bail!("Review decision rating must be in 0 through {MAX_PHOTO_RATING}");
        }
        let before = self.catalog.photo_decision_state(photo_id)?;
        if before.head_sequence != expected_head_sequence {
            bail!(
                "stale Review decision head: expected {expected_head_sequence}, current {}",
                before.head_sequence
            );
        }
        let event = self
            .catalog
            .append_photo_decision_event(&NewPhotoDecisionEvent {
                event_id: Uuid::now_v7().to_string(),
                photo_id,
                occurred_at_unix_ms: current_time_ms()?,
                origin: PhotoDecisionOrigin::Human,
                expected_head_sequence,
                before_flag: before.flag,
                before_rating: before.rating,
                after_flag: photo_flag(flag)?,
                after_rating: rating,
            })?;
        Ok(ffi_photo_decision_receipt(event))
    }
}

fn photo_flag(flag: ffi::FfiDecisionFlag) -> AnyResult<PhotoFlag> {
    match flag {
        ffi::FfiDecisionFlag::Unflagged => Ok(PhotoFlag::Unflagged),
        ffi::FfiDecisionFlag::Picked => Ok(PhotoFlag::Picked),
        ffi::FfiDecisionFlag::Rejected => Ok(PhotoFlag::Rejected),
        _ => bail!("unsupported Review decision flag"),
    }
}

pub(crate) const fn ffi_decision_flag(flag: PhotoFlag) -> ffi::FfiDecisionFlag {
    match flag {
        PhotoFlag::Unflagged => ffi::FfiDecisionFlag::Unflagged,
        PhotoFlag::Picked => ffi::FfiDecisionFlag::Picked,
        PhotoFlag::Rejected => ffi::FfiDecisionFlag::Rejected,
    }
}

fn ffi_photo_decision_state(
    photo_id: PhotoId,
    state: PhotoDecisionState,
) -> ffi::FfiPhotoDecisionState {
    ffi::FfiPhotoDecisionState {
        photo_id: photo_id.to_string(),
        head_sequence: state.head_sequence,
        flag: ffi_decision_flag(state.flag),
        rating: state.rating,
    }
}

fn ffi_photo_decision_receipt(event: PhotoDecisionEvent) -> ffi::FfiReviewDecisionMutationReceipt {
    ffi::FfiReviewDecisionMutationReceipt {
        event_id: event.event_id,
        sequence: event.sequence,
        photo_id: event.photo_id.to_string(),
        occurred_at_unix_ms: event.occurred_at_unix_ms,
        before_head_sequence: event.before_head_sequence,
        before_flag: ffi_decision_flag(event.before_flag),
        before_rating: event.before_rating,
        after_flag: ffi_decision_flag(event.after_flag),
        after_rating: event.after_rating,
    }
}
