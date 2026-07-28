use crate::EntityId;

use super::*;

fn initial_request() -> NewPhotoDecisionEvent {
    NewPhotoDecisionEvent {
        event_id: "decision-1".into(),
        photo_id: PhotoId::new_v7(),
        occurred_at_unix_ms: 1_700_000_000_000,
        origin: PhotoDecisionOrigin::Human,
        expected_head_sequence: 0,
        before_flag: PhotoFlag::Unflagged,
        before_rating: 0,
        after_flag: PhotoFlag::Picked,
        after_rating: 4,
    }
}

#[test]
fn default_state_is_the_implicit_unflagged_zero_rating_head() {
    assert_eq!(
        PhotoDecisionState::default(),
        PhotoDecisionState {
            head_sequence: 0,
            flag: PhotoFlag::Unflagged,
            rating: 0,
        }
    );
    assert!(PhotoDecisionState::new(0, PhotoFlag::Picked, 0).is_err());
    assert!(PhotoDecisionState::new(0, PhotoFlag::Unflagged, 1).is_err());
}

#[test]
fn assigned_event_preserves_before_and_after_without_inventing_state() {
    let request = initial_request();
    let event = request.clone().with_sequence(7).expect("assign sequence");

    assert_eq!(event.before_state().unwrap(), PhotoDecisionState::default());
    assert_eq!(
        event.after_state().unwrap(),
        PhotoDecisionState {
            head_sequence: 7,
            flag: PhotoFlag::Picked,
            rating: 4,
        }
    );
    assert_eq!(event.photo_id, request.photo_id);
    assert_eq!(event.origin, PhotoDecisionOrigin::Human);
}

#[test]
fn invalid_rating_noop_and_non_monotonic_sequence_fail_closed() {
    let mut request = initial_request();
    request.after_rating = 6;
    assert!(matches!(
        request.validate(),
        Err(PhotoDecisionValidationError::InvalidRating { .. })
    ));

    let mut request = initial_request();
    request.after_flag = request.before_flag;
    request.after_rating = request.before_rating;
    assert_eq!(
        request.validate(),
        Err(PhotoDecisionValidationError::NoStateChange)
    );

    let mut request = initial_request();
    request.expected_head_sequence = 8;
    request.before_flag = PhotoFlag::Picked;
    assert!(matches!(
        request.with_sequence(8),
        Err(PhotoDecisionValidationError::NonMonotonicSequence { .. })
    ));
}

#[test]
fn canonical_json_field_order_is_stable() {
    let event = initial_request().with_sequence(1).expect("assign sequence");
    let json = serde_json::to_string(&event).expect("serialize decision event");
    let decoded: PhotoDecisionEvent =
        serde_json::from_str(&json).expect("deserialize decision event");

    assert_eq!(decoded, event);
    assert_eq!(serde_json::to_string(&decoded).unwrap(), json);
    assert!(json.starts_with(r#"{"event_id":"decision-1","sequence":1,"photo_id":"#));
}
