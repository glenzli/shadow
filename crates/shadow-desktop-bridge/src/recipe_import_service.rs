//! Bounded desktop-session state for adaptive Shadow Recipe imports.
//!
//! This registry owns plan tokens, exact destination identity, item attempt
//! generations and staged-proposal authority. Provider and preview execution
//! remain in the existing subject-mask owners; this service only admits and
//! retires those resources as one import lifecycle.

use std::{
    collections::BTreeMap,
    sync::{
        Mutex,
        atomic::{AtomicU64, Ordering},
    },
};

use anyhow::{Result as AnyResult, anyhow, bail};
use shadow_domain::LayerInstanceId;

use crate::{
    recipe_import_plan::{
        PendingSemanticLeaf, RecipeImportNodeSummary, RecipeImportPlan, SemanticLeafResolution,
    },
    recipe_v1::GradeStackDraft,
};

pub(crate) const MAX_ACTIVE_RECIPE_IMPORT_PLANS: usize = 8;

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct RecipeImportTargetIdentity {
    pub(crate) photo_id: String,
    pub(crate) source_path: String,
    pub(crate) base_commit_id: String,
    pub(crate) expected_working_commit_id: String,
    pub(crate) settings: GradeStackDraft,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) enum RecipeImportItemTerminal {
    Pending,
    Running,
    Staged,
    Completed,
    NotFound,
    Unavailable,
    Cancelled,
    Failed,
}

#[derive(Debug, Clone)]
pub(crate) struct RecipeImportItemSnapshot {
    pub(crate) leaf: PendingSemanticLeaf,
    pub(crate) terminal: RecipeImportItemTerminal,
    pub(crate) generation: u64,
    pub(crate) progress_percent: u8,
    pub(crate) detail: String,
    pub(crate) proposal_token: Option<u64>,
    pub(crate) preview: Option<RecipeImportProposalPreview>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RecipeImportProposalPreview {
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) samples: Vec<u8>,
}

#[derive(Debug, Clone)]
pub(crate) struct RecipeImportPlanSnapshot {
    pub(crate) plan_token: u64,
    pub(crate) generation: u64,
    pub(crate) label: String,
    pub(crate) items: Vec<RecipeImportItemSnapshot>,
    pub(crate) nodes: Vec<RecipeImportNodeSummary>,
}

#[derive(Debug, Clone)]
pub(crate) struct RecipeImportExecutionContext {
    pub(crate) plan_token: u64,
    pub(crate) item: PendingSemanticLeaf,
    pub(crate) generation: u64,
    pub(crate) job_token: u64,
    pub(crate) input_session_token: u64,
    pub(crate) target: RecipeImportTargetIdentity,
}

#[derive(Debug)]
pub(crate) struct ClosedRecipeImportResources {
    pub(crate) input_session_token: u64,
    pub(crate) job_tokens: Vec<u64>,
    pub(crate) proposal_tokens: Vec<u64>,
}

#[derive(Debug, Default)]
pub(crate) struct RecipeImportService {
    next_plan_token: AtomicU64,
    plans: Mutex<BTreeMap<u64, RecipeImportEntry>>,
}

#[derive(Debug)]
struct RecipeImportEntry {
    generation: u64,
    target: RecipeImportTargetIdentity,
    plan: RecipeImportPlan,
    input_session_token: u64,
    item_order: Vec<String>,
    items: BTreeMap<String, RecipeImportItemState>,
}

#[derive(Debug)]
struct RecipeImportItemState {
    leaf: PendingSemanticLeaf,
    terminal: RecipeImportItemTerminal,
    generation: u64,
    job_token: Option<u64>,
    proposal_token: Option<u64>,
    preview: Option<RecipeImportProposalPreview>,
    detail: String,
}

impl RecipeImportService {
    pub(crate) fn insert(
        &self,
        target: RecipeImportTargetIdentity,
        plan: RecipeImportPlan,
        input_session_token: u64,
    ) -> AnyResult<RecipeImportPlanSnapshot> {
        let mut plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        if plans.len() >= MAX_ACTIVE_RECIPE_IMPORT_PLANS {
            bail!("too many active semantic Recipe import plans");
        }
        let plan_token = next_token(&self.next_plan_token)?;
        let item_order = plan
            .pending_semantic_leaves()
            .iter()
            .map(|leaf| leaf.item_id.to_string())
            .collect::<Vec<_>>();
        let items = plan
            .pending_semantic_leaves()
            .iter()
            .cloned()
            .map(|leaf| {
                (
                    leaf.item_id.to_string(),
                    RecipeImportItemState {
                        leaf,
                        terminal: RecipeImportItemTerminal::Pending,
                        generation: 0,
                        job_token: None,
                        proposal_token: None,
                        preview: None,
                        detail: String::new(),
                    },
                )
            })
            .collect();
        plans.insert(
            plan_token,
            RecipeImportEntry {
                generation: 1,
                target,
                plan,
                input_session_token,
                item_order,
                items,
            },
        );
        snapshot(plans.get(&plan_token).expect("inserted plan"), plan_token)
    }

    pub(crate) fn snapshot(&self, plan_token: u64) -> AnyResult<RecipeImportPlanSnapshot> {
        let plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        snapshot(plan(&plans, plan_token)?, plan_token)
    }

    pub(crate) fn begin_item(
        &self,
        plan_token: u64,
        item_id: &str,
        job_token: u64,
    ) -> AnyResult<RecipeImportExecutionContext> {
        let mut plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        let entry = plan_mut(&mut plans, plan_token)?;
        let state = entry
            .items
            .get_mut(item_id)
            .ok_or_else(|| anyhow!("semantic Recipe import item is unavailable"))?;
        match state.terminal {
            RecipeImportItemTerminal::Pending
            | RecipeImportItemTerminal::NotFound
            | RecipeImportItemTerminal::Unavailable
            | RecipeImportItemTerminal::Cancelled
            | RecipeImportItemTerminal::Failed => {}
            RecipeImportItemTerminal::Running => {
                bail!("semantic Recipe import item is already running")
            }
            RecipeImportItemTerminal::Staged => {
                bail!("semantic Recipe import item already has a staged proposal")
            }
            RecipeImportItemTerminal::Completed => {
                bail!("semantic Recipe import item is already complete")
            }
        }
        entry.generation = entry
            .generation
            .checked_add(1)
            .ok_or_else(|| anyhow!("Recipe import generation exhausted"))?;
        state.generation = entry.generation;
        state.job_token = Some(job_token);
        state.proposal_token = None;
        state.preview = None;
        state.detail.clear();
        state.terminal = RecipeImportItemTerminal::Running;
        Ok(RecipeImportExecutionContext {
            plan_token,
            item: state.leaf.clone(),
            generation: state.generation,
            job_token,
            input_session_token: entry.input_session_token,
            target: entry.target.clone(),
        })
    }

    pub(crate) fn execution_context(
        &self,
        plan_token: u64,
        item_id: &str,
        job_token: u64,
    ) -> AnyResult<RecipeImportExecutionContext> {
        let plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        let entry = plan(&plans, plan_token)?;
        let state = entry
            .items
            .get(item_id)
            .ok_or_else(|| anyhow!("semantic Recipe import item is unavailable"))?;
        if state.terminal != RecipeImportItemTerminal::Running || state.job_token != Some(job_token)
        {
            bail!("semantic Recipe import item execution is stale");
        }
        Ok(RecipeImportExecutionContext {
            plan_token,
            item: state.leaf.clone(),
            generation: state.generation,
            job_token,
            input_session_token: entry.input_session_token,
            target: entry.target.clone(),
        })
    }

    pub(crate) fn complete_item(
        &self,
        context: &RecipeImportExecutionContext,
        terminal: RecipeImportItemTerminal,
        detail: String,
        proposal_token: Option<u64>,
        preview: Option<RecipeImportProposalPreview>,
    ) -> AnyResult<bool> {
        let mut plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        let Ok(entry) = plan_mut(&mut plans, context.plan_token) else {
            return Ok(false);
        };
        if entry.target != context.target {
            return Ok(false);
        }
        let Some(state) = entry.items.get_mut(&context.item.item_id.to_string()) else {
            return Ok(false);
        };
        if state.terminal != RecipeImportItemTerminal::Running
            || state.generation != context.generation
            || state.job_token != Some(context.job_token)
        {
            return Ok(false);
        }
        state.terminal = terminal;
        state.job_token = None;
        state.detail = detail;
        state.proposal_token = proposal_token;
        state.preview = preview;
        Ok(true)
    }

    pub(crate) fn cancel_item(
        &self,
        plan_token: u64,
        item_id: &str,
        job_token: u64,
    ) -> AnyResult<RecipeImportItemSnapshot> {
        let mut plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        let entry = plan_mut(&mut plans, plan_token)?;
        let state = entry
            .items
            .get_mut(item_id)
            .ok_or_else(|| anyhow!("semantic Recipe import item is unavailable"))?;
        if state.terminal != RecipeImportItemTerminal::Running || state.job_token != Some(job_token)
        {
            bail!("semantic Recipe import item execution is stale");
        }
        state.terminal = RecipeImportItemTerminal::Cancelled;
        state.job_token = None;
        state.detail.clear();
        Ok(item_snapshot(state))
    }

    pub(crate) fn staged_item(
        &self,
        plan_token: u64,
        item_id: &str,
        proposal_token: u64,
        generation: u64,
    ) -> AnyResult<PendingSemanticLeaf> {
        let plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        let state = plan(&plans, plan_token)?
            .items
            .get(item_id)
            .ok_or_else(|| anyhow!("semantic Recipe import item is unavailable"))?;
        if state.terminal != RecipeImportItemTerminal::Staged
            || state.proposal_token != Some(proposal_token)
            || state.generation != generation
        {
            bail!("semantic Recipe import proposal is stale");
        }
        Ok(state.leaf.clone())
    }

    pub(crate) fn resolve_item(
        &self,
        plan_token: u64,
        item_id: &str,
        proposal_token: u64,
        generation: u64,
        resolution: SemanticLeafResolution,
    ) -> AnyResult<RecipeImportItemSnapshot> {
        let mut plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        let entry = plan_mut(&mut plans, plan_token)?;
        let state = entry
            .items
            .get_mut(item_id)
            .ok_or_else(|| anyhow!("semantic Recipe import item is unavailable"))?;
        if state.terminal != RecipeImportItemTerminal::Staged
            || state.proposal_token != Some(proposal_token)
            || state.generation != generation
        {
            bail!("semantic Recipe import proposal is stale");
        }
        entry.plan.resolve_item(resolution)?;
        state.terminal = RecipeImportItemTerminal::Completed;
        state.proposal_token = None;
        state.preview = None;
        state.detail.clear();
        Ok(item_snapshot(state))
    }

    pub(crate) fn exclude_node(
        &self,
        plan_token: u64,
        grade_node_id: &str,
    ) -> AnyResult<(RecipeImportPlanSnapshot, Vec<u64>, Vec<u64>)> {
        let mut plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        let entry = plan_mut(&mut plans, plan_token)?;
        let node_id = grade_node_id.parse::<LayerInstanceId>()?;
        entry.plan.exclude_node(node_id)?;
        let mut jobs = Vec::new();
        let mut discarded = Vec::new();
        for state in entry
            .items
            .values_mut()
            .filter(|state| state.leaf.grade_node_id == node_id)
        {
            if let Some(token) = state.proposal_token.take() {
                discarded.push(token);
            }
            if let Some(token) = state.job_token.take() {
                jobs.push(token);
            }
            state.preview = None;
            if state.terminal != RecipeImportItemTerminal::Completed {
                state.terminal = RecipeImportItemTerminal::Cancelled;
            }
        }
        Ok((snapshot(entry, plan_token)?, jobs, discarded))
    }

    pub(crate) fn finalize(
        &self,
        plan_token: u64,
        target: &RecipeImportTargetIdentity,
    ) -> AnyResult<GradeStackDraft> {
        let plans = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?;
        let entry = plan(&plans, plan_token)?;
        if &entry.target != target {
            bail!("semantic Recipe import destination identity changed");
        }
        entry.plan.finalize()
    }

    pub(crate) fn close(&self, plan_token: u64) -> AnyResult<ClosedRecipeImportResources> {
        let entry = self
            .plans
            .lock()
            .map_err(|_| anyhow!("Recipe import registry is unavailable"))?
            .remove(&plan_token)
            .ok_or_else(|| anyhow!("semantic Recipe import plan is unavailable"))?;
        Ok(ClosedRecipeImportResources {
            input_session_token: entry.input_session_token,
            job_tokens: entry
                .items
                .values()
                .filter_map(|state| state.job_token)
                .collect(),
            proposal_tokens: entry
                .items
                .values()
                .filter_map(|state| state.proposal_token)
                .collect(),
        })
    }
}

fn snapshot(entry: &RecipeImportEntry, plan_token: u64) -> AnyResult<RecipeImportPlanSnapshot> {
    Ok(RecipeImportPlanSnapshot {
        plan_token,
        generation: entry.generation,
        label: entry.plan.label().to_owned(),
        items: entry
            .item_order
            .iter()
            .filter_map(|item_id| entry.items.get(item_id))
            .map(item_snapshot)
            .collect(),
        nodes: entry.plan.node_summaries(),
    })
}

fn item_snapshot(state: &RecipeImportItemState) -> RecipeImportItemSnapshot {
    RecipeImportItemSnapshot {
        leaf: state.leaf.clone(),
        terminal: state.terminal,
        generation: state.generation,
        progress_percent: match state.terminal {
            RecipeImportItemTerminal::Pending => 0,
            RecipeImportItemTerminal::Running => 35,
            RecipeImportItemTerminal::Staged => 85,
            RecipeImportItemTerminal::Completed => 100,
            RecipeImportItemTerminal::NotFound
            | RecipeImportItemTerminal::Unavailable
            | RecipeImportItemTerminal::Cancelled
            | RecipeImportItemTerminal::Failed => 0,
        },
        detail: state.detail.clone(),
        proposal_token: state.proposal_token,
        preview: state.preview.clone(),
    }
}

fn plan(plans: &BTreeMap<u64, RecipeImportEntry>, token: u64) -> AnyResult<&RecipeImportEntry> {
    plans
        .get(&token)
        .ok_or_else(|| anyhow!("semantic Recipe import plan is unavailable"))
}

fn plan_mut(
    plans: &mut BTreeMap<u64, RecipeImportEntry>,
    token: u64,
) -> AnyResult<&mut RecipeImportEntry> {
    plans
        .get_mut(&token)
        .ok_or_else(|| anyhow!("semantic Recipe import plan is unavailable"))
}

fn next_token(sequence: &AtomicU64) -> AnyResult<u64> {
    sequence
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| {
            value.checked_add(1)
        })
        .map(|value| value + 1)
        .map_err(|_| anyhow!("semantic Recipe import token exhausted"))
}

#[cfg(test)]
mod tests;
