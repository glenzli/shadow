//! Edit-session persistence contracts, indexed by responsibility.
//!
//! - `version_history` owns named publication, branching, and version-diff behavior.
//! - `working_state` owns autosave and incompatible/stale working-state recovery.
//! - `checkout_roundtrip` owns non-persistent checkout and full recipe restoration.

mod checkout_roundtrip;
mod variants;
mod version_history;
mod working_state;
