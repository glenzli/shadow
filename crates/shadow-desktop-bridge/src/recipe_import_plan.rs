//! Session-free planning for destination-local Shadow Recipe import.
//!
//! This owner separates reusable authored topology from source-photo managed
//! raster identity. Semantic leaves retain only provider-neutral intent until
//! a caller supplies a verified destination-local managed mask; non-semantic
//! managed leaves remain explicit unsupported blockers. No plan can be
//! finalized while an included node contains either unresolved form.

use std::{collections::HashSet, fmt};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_domain::{
    EntityId, LayerInstanceId, MaskComponentId, MaskComponentOperation, MaskDefinition,
    PhotoCanvasNode, PhotoFoundationNode, RawFoundationDenoise, SemanticMaskIntent,
    ShadowRecipeDocument,
};
use uuid::Uuid;

use crate::recipe_v1::{
    CompositeMaskDraft, GradeNodeDraft, GradeNodeRecipeV1Identity, GradeStackDraft,
    LutEditParameters, MAX_GRADE_NODES, MaskComponentDraft, MaskComponentDraftDefinition,
    PreservedManagedRasterSettings, decode_grade_stack_draft_from_recipe_v1_snapshot,
    validate_grade_stack_draft_recipe_v1,
};

pub(crate) const MAX_RECIPE_IMPORT_COMPONENTS: usize =
    MAX_GRADE_NODES * shadow_domain::MAX_MASK_COMPONENTS;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub(crate) struct RecipeImportItemId(Uuid);

impl RecipeImportItemId {
    fn new() -> Self {
        Self(Uuid::now_v7())
    }

    pub(crate) fn parse(value: &str) -> AnyResult<Self> {
        Ok(Self(
            Uuid::parse_str(value).context("parse semantic Recipe import item identity")?,
        ))
    }
}

impl fmt::Display for RecipeImportItemId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

/// Raster-free semantic work retained by one destination import plan.
#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct PendingSemanticLeaf {
    pub(crate) item_id: RecipeImportItemId,
    pub(crate) grade_node_id: LayerInstanceId,
    pub(crate) component_id: MaskComponentId,
    pub(crate) operation: MaskComponentOperation,
    pub(crate) enabled: bool,
    pub(crate) expansion_percent: i8,
    pub(crate) feather_percent: u8,
    pub(crate) leaf_invert: bool,
    pub(crate) intent: SemanticMaskIntent,
}

/// One destination-local result offered to the exact planned component.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct SemanticLeafResolution {
    pub(crate) item_id: RecipeImportItemId,
    pub(crate) grade_node_id: LayerInstanceId,
    pub(crate) component_id: MaskComponentId,
    pub(crate) definition: MaskDefinition,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RecipeImportNodeSummary {
    pub(crate) grade_node_id: LayerInstanceId,
    pub(crate) label: String,
    pub(crate) semantic_leaf_count: u32,
    pub(crate) unsupported_managed_leaf_count: u32,
    pub(crate) excluded: bool,
}

/// Complete, session-free projection of one Shadow Recipe document.
///
/// The base draft carries adjustment controls and fresh Grade Node identities;
/// mask topology remains in `nodes` until every included semantic leaf has a
/// destination-local raster and every unsupported node has been explicitly
/// excluded.
#[derive(Debug, Clone)]
pub(crate) struct RecipeImportPlan {
    label: String,
    base_draft: GradeStackDraft,
    nodes: Vec<PlannedNode>,
    pending_semantic_leaves: Vec<PendingSemanticLeaf>,
    resolved_item_ids: HashSet<RecipeImportItemId>,
    excluded_item_ids: HashSet<RecipeImportItemId>,
}

#[derive(Debug, Clone)]
struct PlannedNode {
    grade_node_id: LayerInstanceId,
    label: String,
    mask: Option<PlannedMask>,
    excluded: bool,
}

#[derive(Debug, Clone)]
struct PlannedMask {
    components: Vec<PlannedMaskComponent>,
    invert: bool,
}

#[derive(Debug, Clone)]
struct PlannedMaskComponent {
    id: MaskComponentId,
    operation: MaskComponentOperation,
    enabled: bool,
    leaf: PlannedLeaf,
}

#[derive(Debug, Clone)]
enum PlannedLeaf {
    Portable(MaskDefinition),
    PendingSemantic(RecipeImportItemId),
    ResolvedSemantic(MaskDefinition),
    UnsupportedManaged,
}

impl RecipeImportPlan {
    /// Builds one bounded destination plan without retaining source raster
    /// references or source graph/component identities.
    pub(crate) fn from_document(document: &ShadowRecipeDocument) -> AnyResult<Self> {
        let label = document.label().unwrap_or_default().to_owned();
        let mut draft = decode_grade_stack_draft_from_recipe_v1_snapshot(document.snapshot())
            .context("project Shadow Recipe into destination import plan")?;
        if draft.grade_nodes.is_empty() || draft.grade_nodes.len() > MAX_GRADE_NODES {
            bail!("Shadow Recipe import plan requires between 1 and {MAX_GRADE_NODES} Grade Nodes");
        }

        make_photo_private_stages_destination_local(&mut draft);
        let mut nodes = Vec::with_capacity(draft.grade_nodes.len());
        let mut pending_semantic_leaves = Vec::new();
        let mut component_count = 0_usize;
        for node in &mut draft.grade_nodes {
            node.recipe_v1_identity = GradeNodeRecipeV1Identity::new();
            node.shared = None;
            node.fine.lut = LutEditParameters::default();
            let grade_node_id = node.recipe_v1_identity.grade_node_id;
            let mask = take_planned_mask(node, grade_node_id, &mut pending_semantic_leaves)?;
            if let Some(mask) = &mask {
                component_count = checked_component_total(component_count, mask.components.len())?;
            }
            nodes.push(PlannedNode {
                grade_node_id,
                label: node.label.clone(),
                mask,
                excluded: false,
            });
        }

        Ok(Self {
            label,
            base_draft: draft,
            nodes,
            pending_semantic_leaves,
            resolved_item_ids: HashSet::new(),
            excluded_item_ids: HashSet::new(),
        })
    }

    pub(crate) fn label(&self) -> &str {
        &self.label
    }

    pub(crate) fn pending_semantic_leaves(&self) -> &[PendingSemanticLeaf] {
        &self.pending_semantic_leaves
    }

    pub(crate) fn node_summaries(&self) -> Vec<RecipeImportNodeSummary> {
        self.nodes
            .iter()
            .map(|node| {
                let (semantic_leaf_count, unsupported_managed_leaf_count) =
                    node.mask.as_ref().map_or((0, 0), |mask| {
                        mask.components.iter().fold(
                            (0_u32, 0_u32),
                            |(semantic, unsupported), component| match &component.leaf {
                                PlannedLeaf::PendingSemantic(_)
                                | PlannedLeaf::ResolvedSemantic(_) => (semantic + 1, unsupported),
                                PlannedLeaf::UnsupportedManaged => (semantic, unsupported + 1),
                                PlannedLeaf::Portable(_) => (semantic, unsupported),
                            },
                        )
                    });
                RecipeImportNodeSummary {
                    grade_node_id: node.grade_node_id,
                    label: node.label.clone(),
                    semantic_leaf_count,
                    unsupported_managed_leaf_count,
                    excluded: node.excluded,
                }
            })
            .collect()
    }

    /// Replaces the exact pending leaf with one destination-local managed
    /// raster whose semantic and refinement contract is unchanged.
    pub(crate) fn resolve_item(&mut self, resolution: SemanticLeafResolution) -> AnyResult<()> {
        if self.resolved_item_ids.contains(&resolution.item_id) {
            bail!(
                "semantic Recipe import item {} is already resolved",
                resolution.item_id
            );
        }
        if self.excluded_item_ids.contains(&resolution.item_id) {
            bail!(
                "semantic Recipe import item {} belongs to an excluded node",
                resolution.item_id
            );
        }
        let item_index = self
            .pending_semantic_leaves
            .iter()
            .position(|item| item.item_id == resolution.item_id)
            .ok_or_else(|| {
                anyhow!(
                    "semantic Recipe import item {} is unknown",
                    resolution.item_id
                )
            })?;
        let item = self.pending_semantic_leaves[item_index].clone();
        if item.grade_node_id != resolution.grade_node_id
            || item.component_id != resolution.component_id
        {
            bail!(
                "semantic Recipe import resolution identity does not match its planned node and component"
            );
        }
        validate_semantic_resolution(&item, &resolution.definition)?;

        let node = self
            .nodes
            .iter_mut()
            .find(|node| node.grade_node_id == item.grade_node_id)
            .ok_or_else(|| anyhow!("semantic Recipe import target Grade Node is unavailable"))?;
        if node.excluded {
            bail!("semantic Recipe import target Grade Node is excluded");
        }
        let component = node
            .mask
            .as_mut()
            .and_then(|mask| {
                mask.components
                    .iter_mut()
                    .find(|component| component.id == item.component_id)
            })
            .ok_or_else(|| anyhow!("semantic Recipe import target component is unavailable"))?;
        if !matches!(component.leaf, PlannedLeaf::PendingSemantic(id) if id == item.item_id) {
            bail!("semantic Recipe import target component is stale");
        }
        component.leaf = PlannedLeaf::ResolvedSemantic(resolution.definition);
        self.pending_semantic_leaves.remove(item_index);
        self.resolved_item_ids.insert(item.item_id);
        Ok(())
    }

    /// Explicitly removes one whole destination Grade Node from final output.
    /// Remaining nodes retain their original authored order.
    pub(crate) fn exclude_node(&mut self, grade_node_id: LayerInstanceId) -> AnyResult<()> {
        let node = self
            .nodes
            .iter_mut()
            .find(|node| node.grade_node_id == grade_node_id)
            .ok_or_else(|| {
                anyhow!("Shadow Recipe import plan does not contain Grade Node {grade_node_id}")
            })?;
        if node.excluded {
            return Ok(());
        }
        node.excluded = true;
        let excluded_ids = self
            .pending_semantic_leaves
            .iter()
            .filter(|item| item.grade_node_id == grade_node_id)
            .map(|item| item.item_id)
            .collect::<HashSet<_>>();
        self.pending_semantic_leaves
            .retain(|item| item.grade_node_id != grade_node_id);
        self.excluded_item_ids.extend(excluded_ids);
        Ok(())
    }

    /// Materializes only complete included nodes into the ordinary Grade Stack
    /// draft used by persistence, preview, detail and export.
    pub(crate) fn finalize(&self) -> AnyResult<GradeStackDraft> {
        let mut draft = self.base_draft.clone();
        draft.grade_nodes.retain(|grade_node| {
            self.nodes
                .iter()
                .find(|node| node.grade_node_id == grade_node.recipe_v1_identity.grade_node_id)
                .is_some_and(|node| !node.excluded)
        });
        if draft.grade_nodes.is_empty() {
            bail!("Shadow Recipe import plan cannot exclude every Grade Node");
        }
        for grade_node in &mut draft.grade_nodes {
            let planned = self
                .nodes
                .iter()
                .find(|node| node.grade_node_id == grade_node.recipe_v1_identity.grade_node_id)
                .ok_or_else(|| anyhow!("Shadow Recipe import plan lost a Grade Node identity"))?;
            let Some(mask) = &planned.mask else {
                continue;
            };
            let components = mask
                .components
                .iter()
                .map(|component| {
                    let definition = match &component.leaf {
                        PlannedLeaf::Portable(definition)
                        | PlannedLeaf::ResolvedSemantic(definition) => {
                            MaskComponentDraftDefinition::Definition(definition.clone())
                        }
                        PlannedLeaf::PendingSemantic(item_id) => {
                            bail!("semantic Recipe import item {item_id} is unresolved")
                        }
                        PlannedLeaf::UnsupportedManaged => {
                            bail!(
                                "Grade Node {} contains an unsupported non-semantic managed mask",
                                planned.grade_node_id
                            )
                        }
                    };
                    Ok(MaskComponentDraft {
                        id: component.id,
                        operation: component.operation,
                        enabled: component.enabled,
                        definition,
                    })
                })
                .collect::<AnyResult<Vec<_>>>()?;
            grade_node.composite_mask = Some(CompositeMaskDraft {
                components,
                invert: mask.invert,
            });
        }
        validate_grade_stack_draft_recipe_v1(&draft)
            .context("validate finalized semantic Recipe import plan")?;
        Ok(draft)
    }

    #[cfg(test)]
    fn test_projection_json(&self) -> String {
        serde_json::json!({
            "label": &self.label,
            "nodes": self.nodes.iter().map(|node| serde_json::json!({
                "grade_node_id": node.grade_node_id.to_string(),
                "excluded": node.excluded,
                "components": node.mask.as_ref().map_or_else(Vec::new, |mask| mask.components.iter().map(|component| serde_json::json!({
                    "component_id": component.id.to_string(),
                    "operation": format!("{:?}", component.operation),
                    "leaf": match &component.leaf {
                        PlannedLeaf::Portable(_) => "portable",
                        PlannedLeaf::PendingSemantic(_) => "pending_semantic",
                        PlannedLeaf::ResolvedSemantic(_) => "resolved_semantic",
                        PlannedLeaf::UnsupportedManaged => "unsupported_managed",
                    },
                })).collect()),
            })).collect::<Vec<_>>(),
            "pending": self.pending_semantic_leaves.iter().map(|item| serde_json::json!({
                "item_id": item.item_id.to_string(),
                "grade_node_id": item.grade_node_id.to_string(),
                "component_id": item.component_id.to_string(),
                "query": item.intent.query(),
            })).collect::<Vec<_>>(),
        })
        .to_string()
    }
}

fn make_photo_private_stages_destination_local(draft: &mut GradeStackDraft) {
    draft.raw_ai_denoise = RawFoundationDenoise::default();
    draft.foundation = PhotoFoundationNode::default();
    draft.retouch_spots.clear();
    draft.retouch_strokes.clear();
    draft.retouch_enabled = true;
    draft.image_completions.clear();
    draft.image_completion_enabled = true;
    draft.liquify = None;
    draft.canvas = PhotoCanvasNode::identity();
}

fn take_planned_mask(
    node: &mut GradeNodeDraft,
    grade_node_id: LayerInstanceId,
    pending: &mut Vec<PendingSemanticLeaf>,
) -> AnyResult<Option<PlannedMask>> {
    let composite = node.composite_mask.take();
    let local = node.local_mask.take();
    let preserved = node.preserved_managed_raster.take();
    let representation_count = usize::from(composite.is_some())
        + usize::from(local.is_some())
        + usize::from(preserved.is_some());
    if representation_count > 1 {
        bail!("Shadow Recipe Grade Node has multiple local-mask representations");
    }
    if let Some(composite) = composite {
        if composite.components.is_empty()
            || composite.components.len() > shadow_domain::MAX_MASK_COMPONENTS
        {
            bail!("Shadow Recipe composite mask component count is outside the supported bound");
        }
        let components = composite
            .components
            .into_iter()
            .map(|component| {
                let component_id = MaskComponentId::new_v7();
                planned_component(
                    grade_node_id,
                    component_id,
                    component.operation,
                    component.enabled,
                    component.definition,
                    pending,
                )
            })
            .collect::<AnyResult<Vec<_>>>()?;
        return Ok(Some(PlannedMask {
            components,
            invert: composite.invert,
        }));
    }
    if let Some(local) = local {
        if let MaskDefinition::Composite { composite } = local {
            let components = composite
                .components()
                .iter()
                .map(|component| {
                    planned_component(
                        grade_node_id,
                        MaskComponentId::new_v7(),
                        component.operation(),
                        component.enabled(),
                        MaskComponentDraftDefinition::Definition(component.definition().clone()),
                        pending,
                    )
                })
                .collect::<AnyResult<Vec<_>>>()?;
            return Ok(Some(PlannedMask {
                components,
                invert: composite.invert(),
            }));
        }
        return Ok(Some(PlannedMask {
            components: vec![planned_component(
                grade_node_id,
                MaskComponentId::new_v7(),
                MaskComponentOperation::Base,
                true,
                MaskComponentDraftDefinition::Definition(local),
                pending,
            )?],
            invert: false,
        }));
    }
    if let Some(settings) = preserved {
        return Ok(Some(PlannedMask {
            components: vec![planned_component(
                grade_node_id,
                MaskComponentId::new_v7(),
                MaskComponentOperation::Base,
                true,
                MaskComponentDraftDefinition::PreservedManagedRaster(settings),
                pending,
            )?],
            invert: false,
        }));
    }
    Ok(None)
}

fn planned_component(
    grade_node_id: LayerInstanceId,
    component_id: MaskComponentId,
    operation: MaskComponentOperation,
    enabled: bool,
    definition: MaskComponentDraftDefinition,
    pending: &mut Vec<PendingSemanticLeaf>,
) -> AnyResult<PlannedMaskComponent> {
    let leaf = match definition {
        MaskComponentDraftDefinition::Definition(MaskDefinition::ManagedRaster {
            semantic_intent,
            expansion_percent,
            feather_percent,
            invert,
            ..
        }) => planned_managed_leaf(
            grade_node_id,
            component_id,
            operation,
            enabled,
            PreservedManagedRasterSettings {
                expansion_percent,
                feather_percent,
                invert,
                semantic_intent,
            },
            pending,
        ),
        MaskComponentDraftDefinition::Definition(definition) => PlannedLeaf::Portable(definition),
        MaskComponentDraftDefinition::PreservedManagedRaster(settings) => planned_managed_leaf(
            grade_node_id,
            component_id,
            operation,
            enabled,
            settings,
            pending,
        ),
    };
    Ok(PlannedMaskComponent {
        id: component_id,
        operation,
        enabled,
        leaf,
    })
}

fn planned_managed_leaf(
    grade_node_id: LayerInstanceId,
    component_id: MaskComponentId,
    operation: MaskComponentOperation,
    enabled: bool,
    settings: PreservedManagedRasterSettings,
    pending: &mut Vec<PendingSemanticLeaf>,
) -> PlannedLeaf {
    let Some(intent) = settings.semantic_intent else {
        return PlannedLeaf::UnsupportedManaged;
    };
    let item_id = RecipeImportItemId::new();
    pending.push(PendingSemanticLeaf {
        item_id,
        grade_node_id,
        component_id,
        operation,
        enabled,
        expansion_percent: settings.expansion_percent,
        feather_percent: settings.feather_percent,
        leaf_invert: settings.invert,
        intent,
    });
    PlannedLeaf::PendingSemantic(item_id)
}

fn validate_semantic_resolution(
    item: &PendingSemanticLeaf,
    definition: &MaskDefinition,
) -> AnyResult<()> {
    let MaskDefinition::ManagedRaster {
        semantic_intent,
        expansion_percent,
        feather_percent,
        invert,
        ..
    } = definition
    else {
        bail!("semantic Recipe import resolution must be a managed raster");
    };
    if semantic_intent.as_ref() != Some(&item.intent)
        || *expansion_percent != item.expansion_percent
        || *feather_percent != item.feather_percent
        || *invert != item.leaf_invert
    {
        bail!("semantic Recipe import resolution does not match its planned intent and refinement");
    }
    Ok(())
}

fn checked_component_total(current: usize, added: usize) -> AnyResult<usize> {
    let total = current
        .checked_add(added)
        .ok_or_else(|| anyhow!("Shadow Recipe import component count overflowed"))?;
    if total > MAX_RECIPE_IMPORT_COMPONENTS {
        bail!(
            "Shadow Recipe import contains {total} mask components, exceeding {MAX_RECIPE_IMPORT_COMPONENTS}"
        );
    }
    Ok(total)
}

#[cfg(test)]
mod tests;
