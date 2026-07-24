//! Linearizable host-side lifecycle for bounded edit-preview requests.
//!
//! This first cancellation layer deliberately does not interrupt native CPU or
//! GPU work already executing in `shadow-image`. It establishes the terminal
//! ownership rule at the desktop boundary: cancellation and publication race
//! exactly once, and whichever atomically claims the request first determines
//! whether pixels and durable cache artifacts may escape.

use std::{
    collections::VecDeque,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU8, AtomicU64, Ordering},
    },
};

const ACTIVE: u8 = 0;
const CANCELLED: u8 = 1;
const TERMINAL_CLAIMED: u8 = 2;

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum PreviewAdmission {
    Active,
    Cancelled,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum PreviewTerminalClaim {
    Completed,
    Cancelled,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum PreviewRenderRegistryError {
    UnknownToken,
    TerminalAlreadyClaimed,
    Poisoned,
}

#[derive(Debug)]
struct PreviewRenderEntry {
    token: u64,
    state: Arc<AtomicU8>,
}

/// A session-local registry. Qt intentionally keeps at most one overview
/// render in flight, but a small registry makes cancellation safe across the
/// queueing boundary and keeps the contract usable if worker scheduling later
/// becomes concurrent.
#[derive(Debug, Default)]
pub(crate) struct PreviewRenderRegistry {
    next_token: AtomicU64,
    entries: Mutex<VecDeque<PreviewRenderEntry>>,
}

impl PreviewRenderRegistry {
    pub(crate) fn begin(&self) -> Result<u64, PreviewRenderRegistryError> {
        let token = loop {
            let candidate = self
                .next_token
                .fetch_add(1, Ordering::SeqCst)
                .wrapping_add(1);
            if candidate != 0 {
                break candidate;
            }
        };
        let mut entries = self
            .entries
            .lock()
            .map_err(|_| PreviewRenderRegistryError::Poisoned)?;
        // Completed entries are no longer observable. Cancelled entries remain
        // until their queued worker acknowledges cancellation, so a task that
        // has not started yet cannot accidentally become an unknown failure.
        entries.retain(|entry| entry.state.load(Ordering::Acquire) != TERMINAL_CLAIMED);
        entries.push_back(PreviewRenderEntry {
            token,
            state: Arc::new(AtomicU8::new(ACTIVE)),
        });
        Ok(token)
    }

    pub(crate) fn admission(
        &self,
        token: u64,
    ) -> Result<PreviewAdmission, PreviewRenderRegistryError> {
        let state = self.state(token)?;
        match state.load(Ordering::Acquire) {
            ACTIVE => Ok(PreviewAdmission::Active),
            CANCELLED => Ok(PreviewAdmission::Cancelled),
            TERMINAL_CLAIMED => Err(PreviewRenderRegistryError::TerminalAlreadyClaimed),
            _ => unreachable!("preview registry stores only declared lifecycle states"),
        }
    }

    /// Attempts to make cancellation the unique terminal owner. Returning
    /// false means completion already won or the token is not registered.
    pub(crate) fn cancel(&self, token: u64) -> bool {
        let Ok(state) = self.state(token) else {
            return false;
        };
        state
            .compare_exchange(ACTIVE, CANCELLED, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
    }

    /// Claims the terminal outcome after native work returns. A preceding
    /// cancellation always wins and therefore forbids result validation,
    /// durable cache admission, and UI publication at the host boundary.
    pub(crate) fn claim_terminal(
        &self,
        token: u64,
    ) -> Result<PreviewTerminalClaim, PreviewRenderRegistryError> {
        let state = self.state(token)?;
        match state.compare_exchange(
            ACTIVE,
            TERMINAL_CLAIMED,
            Ordering::AcqRel,
            Ordering::Acquire,
        ) {
            Ok(ACTIVE) => Ok(PreviewTerminalClaim::Completed),
            Err(CANCELLED) => {
                state.store(TERMINAL_CLAIMED, Ordering::Release);
                Ok(PreviewTerminalClaim::Cancelled)
            }
            Err(TERMINAL_CLAIMED) => Err(PreviewRenderRegistryError::TerminalAlreadyClaimed),
            Ok(_) | Err(_) => {
                unreachable!("preview registry stores only declared lifecycle states")
            }
        }
    }

    fn state(&self, token: u64) -> Result<Arc<AtomicU8>, PreviewRenderRegistryError> {
        if token == 0 {
            return Err(PreviewRenderRegistryError::UnknownToken);
        }
        let entries = self
            .entries
            .lock()
            .map_err(|_| PreviewRenderRegistryError::Poisoned)?;
        entries
            .iter()
            .find(|entry| entry.token == token)
            .map(|entry| Arc::clone(&entry.state))
            .ok_or(PreviewRenderRegistryError::UnknownToken)
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Barrier;

    use super::*;

    #[test]
    fn cancellation_claimed_first_forbids_completion() {
        let registry = PreviewRenderRegistry::default();
        let token = registry.begin().expect("begin request");

        assert!(registry.cancel(token));
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

        assert_eq!(
            registry.admission(token).expect("active admission"),
            PreviewAdmission::Active
        );
        assert_eq!(
            registry.claim_terminal(token).expect("claim completion"),
            PreviewTerminalClaim::Completed
        );
        assert!(!registry.cancel(token));
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
            assert!(
                !registry.cancel(token),
                "neither terminal outcome may be replaced by a late cancellation"
            );
        }
    }
}
