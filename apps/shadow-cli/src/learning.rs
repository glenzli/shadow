//! Explicit operator-owned evidence capture and bounded local readiness inspection.
//! No inference, feature extraction, image export, or model update occurs here.

use std::{
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Result, bail};
use shadow_ai::{
    EditExampleIntent, EditExampleOrigin, FeedbackAction, LearningScope, NewFeedbackEvent,
    NewFeedbackForgetFact, PresentationContext,
};
use shadow_catalog::Catalog;
use shadow_domain::{PhotoId, RecipeCommitId};
use uuid::Uuid;

pub(super) fn run_if_requested(arguments: &[String]) -> Result<bool> {
    let Some(command) = arguments.first() else {
        return Ok(false);
    };
    if !command.starts_with("learning-") {
        return Ok(false);
    }
    match arguments {
        [_, path, scope, after] if command == "learning-report" => {
            let catalog = open_existing(path)?;
            let scope = parse_scope(scope)?;
            let after: u64 = after.parse()?;
            let page = catalog.learning_evidence_page(&scope, after, 512)?;
            println!("{}", serde_json::to_string_pretty(&page)?);
        }
        [_, path, scope, photo, baseline, approved, origin, intent]
            if command == "learning-confirm-edit" =>
        {
            let mut catalog = open_existing(path)?;
            let photo_id: PhotoId = photo.parse().map_err(anyhow::Error::msg)?;
            let baseline_recipe: RecipeCommitId = baseline.parse().map_err(anyhow::Error::msg)?;
            let approved_recipe: RecipeCommitId = approved.parse().map_err(anyhow::Error::msg)?;
            for commit in [baseline_recipe, approved_recipe] {
                if catalog.recipe_commit(photo_id, commit)?.is_none() {
                    bail!("both Recipe commits must exist and belong to the photo");
                }
            }
            let origin = match origin.as_str() {
                "manual" => EditExampleOrigin::Manual,
                "imported" => EditExampleOrigin::Imported,
                "assisted" => EditExampleOrigin::Assisted,
                "mixed" => EditExampleOrigin::Mixed,
                "unknown" => EditExampleOrigin::Unknown,
                _ => bail!("origin: manual|imported|assisted|mixed|unknown"),
            };
            let intent = match intent.as_str() {
                "style" => EditExampleIntent::PersonalStyle,
                "correction" => EditExampleIntent::TechnicalCorrection,
                _ => bail!("intent: style|correction"),
            };
            let event = catalog.append_feedback_event(&NewFeedbackEvent {
                event_id: Uuid::now_v7().to_string(),
                occurred_at_unix_ms: now_ms()?,
                scope: parse_scope(scope)?,
                presentation: PresentationContext {
                    session_id: format!("operator-{}", Uuid::now_v7()),
                    group_id: None,
                    candidates: vec![],
                    active_model: None,
                },
                action: FeedbackAction::EditExampleConfirmed {
                    photo_id,
                    baseline_recipe,
                    approved_recipe,
                    origin,
                    intent,
                },
            })?;
            println!("{}", serde_json::to_string_pretty(&event)?);
        }
        [_, path, event_id] if command == "learning-forget" => {
            let mut catalog = open_existing(path)?;
            let fact = catalog.append_feedback_forget_fact(&NewFeedbackForgetFact {
                fact_id: Uuid::now_v7().to_string(),
                target_event_id: event_id.clone(),
                occurred_at_unix_ms: now_ms()?,
                reason: Some("explicit operator request to exclude learning evidence".into()),
            })?;
            println!("{}", serde_json::to_string_pretty(&fact)?);
        }
        _ => bail!(
            "usage: learning-report <catalog> <global|project:id> <after-sequence>; learning-confirm-edit <catalog> <scope> <photo-id> <baseline-commit> <approved-commit> <origin> <style|correction>; learning-forget <catalog> <event-id>"
        ),
    }
    Ok(true)
}

fn open_existing(path: &str) -> Result<Catalog> {
    let path = Path::new(path);
    if !path.is_file() {
        bail!("learning commands require an existing Catalog file");
    }
    // Uses normal supported Catalog opening/migration; never resets an incompatible Catalog.
    Ok(Catalog::open(path)?)
}

fn parse_scope(value: &str) -> Result<LearningScope> {
    if value == "global" {
        return Ok(LearningScope::Global);
    }
    if let Some(project_id) = value.strip_prefix("project:")
        && !project_id.trim().is_empty()
        && project_id.len() <= 256
    {
        return Ok(LearningScope::Project {
            project_id: project_id.into(),
        });
    }
    bail!("scope must be global or project:<non-empty-id>")
}

fn now_ms() -> Result<i64> {
    Ok(i64::try_from(
        SystemTime::now().duration_since(UNIX_EPOCH)?.as_millis(),
    )?)
}
