//! Linearizable lifecycle and native stop signal for edit-preview requests.
//!
//! Cancellation and publication race exactly once at this desktop boundary.
//! When cancellation wins, the same registry entry signals both AI foundation
//! resolution and the native renderer so source preparation and cooperative
//! CPU/Metal checkpoints can stop early. When completion wins, a later host
//! cancellation cannot reach either handle.

use std::{
    collections::VecDeque,
    fmt,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU8, AtomicU64, Ordering},
    },
};

use shadow_ai::CancellationToken;
use shadow_bridge::EditPreviewCancellation;

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

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) enum PreviewRenderRegistryError {
    UnknownToken,
    TerminalAlreadyClaimed,
    Poisoned,
    NativeCancellation(String),
}

struct PreviewRenderEntry {
    token: u64,
    state: Arc<AtomicU8>,
    native_cancellation: EditPreviewCancellation,
    foundation_cancellation: CancellationToken,
}

impl fmt::Debug for PreviewRenderEntry {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("PreviewRenderEntry")
            .field("token", &self.token)
            .field("state", &self.state.load(Ordering::Acquire))
            .field("native_cancellation", &"<opaque native stop handle>")
            .field("foundation_cancellation", &"<AI source stop handle>")
            .finish()
    }
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
        let native_cancellation = EditPreviewCancellation::new()
            .map_err(|error| PreviewRenderRegistryError::NativeCancellation(error.to_string()))?;
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
            native_cancellation,
            foundation_cancellation: CancellationToken::default(),
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
    /// false means completion already won or the token is not registered. The
    /// native stop signal is sent only after this host CAS succeeds, so a late
    /// cancellation can never stop a render whose completion already won.
    pub(crate) fn cancel(&self, token: u64) -> bool {
        let Ok((state, native_cancellation, foundation_cancellation)) = self.entry_handles(token)
        else {
            return false;
        };
        if state
            .compare_exchange(ACTIVE, CANCELLED, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return false;
        }
        let native_stop_won = native_cancellation.cancel();
        foundation_cancellation.cancel();
        debug_assert!(
            native_stop_won,
            "only the registry may signal a preview's native cancellation handle"
        );
        true
    }

    /// Returns the native stop handle paired with `token`.
    ///
    /// Workers clone this before entering the native renderer. The lifecycle
    /// state remains owned by this registry; callers must not signal the handle
    /// directly.
    pub(crate) fn cancellation(
        &self,
        token: u64,
    ) -> Result<EditPreviewCancellation, PreviewRenderRegistryError> {
        self.entry_handles(token)
            .map(|(_, native_cancellation, _)| native_cancellation)
    }

    /// Returns the AI/source-preparation stop handle paired with `token`.
    ///
    /// The registry remains its only signalling owner. Workers may clone this
    /// handle only to pass it into cancellable foundation resolution.
    pub(crate) fn foundation_cancellation(
        &self,
        token: u64,
    ) -> Result<CancellationToken, PreviewRenderRegistryError> {
        self.entry_handles(token)
            .map(|(_, _, foundation_cancellation)| foundation_cancellation)
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
            Err(CANCELLED) => match state.compare_exchange(
                CANCELLED,
                TERMINAL_CLAIMED,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(CANCELLED) => Ok(PreviewTerminalClaim::Cancelled),
                Err(TERMINAL_CLAIMED) => Err(PreviewRenderRegistryError::TerminalAlreadyClaimed),
                Ok(_) | Err(_) => {
                    unreachable!("preview registry lifecycle states advance monotonically")
                }
            },
            Err(TERMINAL_CLAIMED) => Err(PreviewRenderRegistryError::TerminalAlreadyClaimed),
            Ok(_) | Err(_) => {
                unreachable!("preview registry stores only declared lifecycle states")
            }
        }
    }

    fn state(&self, token: u64) -> Result<Arc<AtomicU8>, PreviewRenderRegistryError> {
        self.entry_handles(token).map(|(state, _, _)| state)
    }

    fn entry_handles(
        &self,
        token: u64,
    ) -> Result<
        (Arc<AtomicU8>, EditPreviewCancellation, CancellationToken),
        PreviewRenderRegistryError,
    > {
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
            .map(|entry| {
                (
                    Arc::clone(&entry.state),
                    entry.native_cancellation.clone(),
                    entry.foundation_cancellation.clone(),
                )
            })
            .ok_or(PreviewRenderRegistryError::UnknownToken)
    }
}

#[cfg(test)]
mod tests;
