use std::sync::Barrier;

use super::*;

#[test]
fn cancellation_claimed_first_forbids_completion() {
    let registry = PreviewRenderRegistry::default();
    let token = registry.begin().expect("begin request");
    let native = registry.cancellation(token).expect("native cancellation");
    let foundation = registry
        .foundation_cancellation(token)
        .expect("foundation cancellation");

    assert!(registry.cancel(token));
    assert!(
        foundation.is_cancelled(),
        "winning host cancellation must reach AI source preparation"
    );
    assert!(
        !native.cancel(),
        "winning host cancellation must signal the native stop handle exactly once"
    );
    assert_eq!(
        registry.admission(token).expect("cancelled admission"),
        PreviewAdmission::Cancelled
    );
    assert_eq!(
        registry
            .claim_terminal(token)
            .expect("acknowledge cancellation"),
        PreviewTerminalClaim::Cancelled
    );
    assert!(!registry.cancel(token));
}

#[test]
fn completion_claimed_first_rejects_late_cancellation() {
    let registry = PreviewRenderRegistry::default();
    let token = registry.begin().expect("begin request");
    let native = registry.cancellation(token).expect("native cancellation");
    let foundation = registry
        .foundation_cancellation(token)
        .expect("foundation cancellation");

    assert_eq!(
        registry.admission(token).expect("active admission"),
        PreviewAdmission::Active
    );
    assert_eq!(
        registry.claim_terminal(token).expect("claim completion"),
        PreviewTerminalClaim::Completed
    );
    assert!(!registry.cancel(token));
    assert!(
        !foundation.is_cancelled(),
        "late host cancellation must not stop completed AI source preparation"
    );
    assert!(
        native.cancel(),
        "late host cancellation must not have signalled native work after completion won"
    );
    assert_eq!(
        registry.claim_terminal(token),
        Err(PreviewRenderRegistryError::TerminalAlreadyClaimed)
    );
}

#[test]
fn tokens_are_nonzero_unique_and_unknown_tokens_fail_closed() {
    let registry = PreviewRenderRegistry::default();
    let first = registry.begin().expect("first request");
    let second = registry.begin().expect("second request");

    assert_ne!(first, 0);
    assert_ne!(first, second);
    assert_eq!(
        registry.admission(0),
        Err(PreviewRenderRegistryError::UnknownToken)
    );
    assert_eq!(
        registry.claim_terminal(u64::MAX),
        Err(PreviewRenderRegistryError::UnknownToken)
    );
}

#[test]
fn registry_prunes_acknowledged_terminals_without_losing_live_requests() {
    let registry = PreviewRenderRegistry::default();
    let completed = registry.begin().expect("completed request");
    let live = registry.begin().expect("live request");
    assert_eq!(
        registry
            .claim_terminal(completed)
            .expect("complete first request"),
        PreviewTerminalClaim::Completed
    );

    let next = registry.begin().expect("next request");
    assert_eq!(
        registry.admission(completed),
        Err(PreviewRenderRegistryError::UnknownToken)
    );
    assert_eq!(
        registry.admission(live).expect("live request retained"),
        PreviewAdmission::Active
    );
    assert_eq!(
        registry.admission(next).expect("new request retained"),
        PreviewAdmission::Active
    );
}

#[test]
fn cancellation_and_completion_have_exactly_one_winner_under_race() {
    let registry = Arc::new(PreviewRenderRegistry::default());
    for _ in 0..128 {
        let token = registry.begin().expect("begin raced request");
        let native = registry.cancellation(token).expect("native cancellation");
        let foundation = registry
            .foundation_cancellation(token)
            .expect("foundation cancellation");
        let barrier = Arc::new(Barrier::new(3));
        let cancel_registry = Arc::clone(&registry);
        let cancel_barrier = Arc::clone(&barrier);
        let cancel = std::thread::spawn(move || {
            cancel_barrier.wait();
            cancel_registry.cancel(token)
        });
        let complete_registry = Arc::clone(&registry);
        let complete_barrier = Arc::clone(&barrier);
        let complete = std::thread::spawn(move || {
            complete_barrier.wait();
            complete_registry
                .claim_terminal(token)
                .expect("one terminal claim")
        });
        barrier.wait();
        let cancellation_won = cancel.join().expect("join cancellation");
        let terminal = complete.join().expect("join completion");
        assert_eq!(
            cancellation_won,
            terminal == PreviewTerminalClaim::Cancelled
        );
        assert_eq!(
            native.cancel(),
            !cancellation_won,
            "native stop must be signalled iff host cancellation won"
        );
        assert_eq!(
            foundation.is_cancelled(),
            cancellation_won,
            "AI source stop must have the same terminal winner as native rendering"
        );
        assert!(
            !registry.cancel(token),
            "neither terminal outcome may be replaced by a late cancellation"
        );
    }
}

#[test]
fn cancelled_request_has_exactly_one_acknowledgement_owner() {
    let registry = Arc::new(PreviewRenderRegistry::default());
    for _ in 0..128 {
        let token = registry.begin().expect("begin cancelled request");
        assert!(registry.cancel(token));
        let barrier = Arc::new(Barrier::new(3));
        let first_registry = Arc::clone(&registry);
        let first_barrier = Arc::clone(&barrier);
        let first = std::thread::spawn(move || {
            first_barrier.wait();
            first_registry.claim_terminal(token)
        });
        let second_registry = Arc::clone(&registry);
        let second_barrier = Arc::clone(&barrier);
        let second = std::thread::spawn(move || {
            second_barrier.wait();
            second_registry.claim_terminal(token)
        });
        barrier.wait();
        let outcomes = [
            first.join().expect("join first acknowledgement"),
            second.join().expect("join second acknowledgement"),
        ];
        assert_eq!(
            outcomes
                .iter()
                .filter(|outcome| { **outcome == Ok(PreviewTerminalClaim::Cancelled) })
                .count(),
            1,
            "exactly one worker may acknowledge a cancelled request"
        );
        assert_eq!(
            outcomes
                .iter()
                .filter(|outcome| {
                    **outcome == Err(PreviewRenderRegistryError::TerminalAlreadyClaimed)
                })
                .count(),
            1,
            "the losing acknowledgement must observe the claimed terminal"
        );
    }
}
