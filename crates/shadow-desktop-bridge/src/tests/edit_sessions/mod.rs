//! Edit-session persistence contracts, indexed by responsibility.
//!
//! - `version_history` owns named publication, branching, and version-diff behavior.
//! - `working_state` owns autosave and incompatible/stale working-state recovery.
//! - `checkout_roundtrip` owns non-persistent checkout and full recipe restoration.

// `concurrent_history` exercises simultaneous writers and long edit/reopen sequences.
// `history_scale` is an opt-in synthetic history projection measurement.
mod checkout_roundtrip;
mod concurrent_history;
mod history_scale;
mod variants;
mod version_history;
mod working_state;
