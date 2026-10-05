//! Headless access to the existing authoritative Review decision ledger.
//!
//! The request carries a user-directed assertion; it does not grant an agent
//! authority to infer human decisions. No parallel ledger or Recipe is created.

use anyhow::Result;
use serde_json::{Value, json};
use shadow_domain::{PhotoFlag, PhotoId};

use super::{fault, protocol::DecisionProvenance};
use crate::DesktopSession;

fn photo_id(value: &str) -> Result<PhotoId> {
    value
        .parse()
        .map_err(|_| fault("invalid_request", "invalid photo_id"))
}

pub(super) fn state(session: &DesktopSession, photo: &str) -> Result<Value> {
    let photo = photo_id(photo)?;
    let state = session.catalog.photo_decision_state(photo)?;
    Ok(json!({
        "photo_id": photo, "head_sequence": state.head_sequence,
        "flag": state.flag, "rating": state.rating
    }))
}

pub(super) fn set(
    session: &DesktopSession,
    photo: &str,
    expected_head_sequence: u64,
    flag: PhotoFlag,
    rating: u8,
    provenance: &DecisionProvenance,
) -> Result<Value> {
    photo_id(photo)?;
    // Deserialization admits only an explicit user-directed declaration. The
    // shared service validates the transition and commits through Catalog CAS.
    let event =
        session
            .review
            .set_photo_decision_event(photo, expected_head_sequence, flag, rating)?;
    Ok(json!({
        "event": event,
        "request_provenance": provenance,
        "request_provenance_storage": "response_only"
    }))
}

pub(super) fn history(
    session: &DesktopSession,
    photo: &str,
    after: u64,
    limit: u32,
) -> Result<Value> {
    if !(1..=100).contains(&limit) || i64::try_from(after).is_err() {
        return Err(fault(
            "invalid_request",
            "decision history requires limit 1..=100 and a nonnegative SQLite sequence",
        ));
    }
    let photo = photo_id(photo)?;
    let page = session
        .catalog
        .photo_decision_events_after(photo, after, limit as usize)?;
    let next_cursor = page
        .events
        .last()
        .filter(|_| page.has_more)
        .map(|event| event.sequence);
    Ok(json!({
        "photo_id": photo, "events": page.events,
        "has_more": page.has_more, "next_cursor": next_cursor,
        "executor_provenance": "not_recorded"
    }))
}

#[cfg(test)]
mod tests;
