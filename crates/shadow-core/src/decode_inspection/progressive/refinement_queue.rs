//! Bounded pending refinement, latest visible identities, and background fairness.
//! Only scheduling changes: every accepted job keeps its original completion and source guard.

use super::RefinementJob;
use shadow_domain::RepresentationId;
use std::{
    collections::{HashSet, VecDeque},
    sync::{Arc, Condvar, Mutex},
    time::Duration,
};

const QUEUE_CAPACITY: usize = 16_384;
pub const MAX_VISIBLE_REFINEMENTS: usize = 512;
const PRIORITY_BURST: usize = 8;

#[derive(Debug, Default)]
struct Visible {
    selected: Option<RepresentationId>,
    representations: HashSet<RepresentationId>,
}

/// One import's transient viewport hint. Replacing it never cancels durable work.
#[derive(Debug, Clone, Default)]
pub struct RefinementPriority(Arc<Mutex<Visible>>);

impl RefinementPriority {
    pub fn replace(&self, selected: Option<RepresentationId>, visible: &[RepresentationId]) {
        let mut state = self
            .0
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        state.selected = selected;
        state.representations = visible
            .iter()
            .take(MAX_VISIBLE_REFINEMENTS)
            .copied()
            .collect();
    }
}

#[derive(Default)]
struct QueueState {
    jobs: VecDeque<RefinementJob>,
    closed: bool,
    priority_streak: usize,
}

pub(super) struct RefinementQueue {
    state: Mutex<QueueState>,
    changed: Condvar,
    priority: RefinementPriority,
}

impl RefinementQueue {
    pub(super) fn new(priority: RefinementPriority) -> Self {
        Self {
            state: Mutex::default(),
            changed: Condvar::new(),
            priority,
        }
    }

    pub(super) fn push(&self, job: RefinementJob) -> Result<(), Box<RefinementJob>> {
        let mut state = self
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        while state.jobs.len() >= QUEUE_CAPACITY
            && !state.closed
            && !job.inspection.cancellation.is_cancelled()
        {
            state = self
                .changed
                .wait_timeout(state, Duration::from_millis(10))
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .0;
        }
        if state.closed || job.inspection.cancellation.is_cancelled() {
            return Err(Box::new(job));
        }
        state.jobs.push_back(job);
        self.changed.notify_one();
        Ok(())
    }

    pub(super) fn pop(&self) -> Option<RefinementJob> {
        let mut state = self
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        loop {
            if !state.jobs.is_empty() {
                let visible = self
                    .priority
                    .0
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner);
                let preferred = if state.priority_streak < PRIORITY_BURST
                    && (visible.selected.is_some() || !visible.representations.is_empty())
                {
                    let selected = visible.selected.and_then(|id| {
                        state
                            .jobs
                            .iter()
                            .position(|job| job.inspection.request.representation_id == id)
                    });
                    selected.or_else(|| {
                        state.jobs.iter().position(|job| {
                            visible
                                .representations
                                .contains(&job.inspection.request.representation_id)
                        })
                    })
                } else {
                    None
                };
                state.priority_streak = if preferred.is_some() {
                    state.priority_streak + 1
                } else {
                    0
                };
                let job = state.jobs.remove(preferred.unwrap_or(0));
                self.changed.notify_all();
                return job;
            }
            if state.closed {
                return None;
            }
            state = self
                .changed
                .wait(state)
                .unwrap_or_else(std::sync::PoisonError::into_inner);
        }
    }

    pub(super) fn close(&self) {
        self.state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .closed = true;
        self.changed.notify_all();
    }
}

#[cfg(test)]
mod tests;
