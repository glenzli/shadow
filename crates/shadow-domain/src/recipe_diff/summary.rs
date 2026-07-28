//! Stable aggregate counts and compact fallback presentation for Recipe diffs.

use std::fmt;

use serde::Serialize;

use super::model::{LayerContentDiff, RecipeDiff};

impl RecipeDiff {
    pub fn summary(&self) -> RecipeDiffSummary {
        RecipeDiffSummary::from(self)
    }

    /// Produces a compact, deterministic English fallback. UI code should use
    /// [`Self::summary`] when it needs localized labels.
    pub fn compact_summary(&self) -> String {
        self.summary().to_string()
    }
}

/// Stable aggregate counts suitable for a history or comparison UI.
#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Serialize)]
pub struct RecipeDiffSummary {
    pub recipe_schema_changed: bool,
    pub input_settings_changed: bool,
    pub layers_added: usize,
    pub layers_removed: usize,
    pub layers_moved: usize,
    pub layers_modified: usize,
    pub layer_instance_fields_changed: usize,
    pub layer_content_replacements: usize,
    pub shared_layer_targets_changed: usize,
    pub shared_revision_selectors_changed: usize,
    pub graph_schemas_changed: usize,
    pub graph_inputs_changed: usize,
    pub graph_outputs_changed: usize,
    pub nodes_added: usize,
    pub nodes_removed: usize,
    pub nodes_modified: usize,
    pub operation_contracts_changed: usize,
    pub node_inputs_changed: usize,
    pub node_parameters_changed: usize,
    pub node_masks_changed: usize,
}

impl From<&RecipeDiff> for RecipeDiffSummary {
    fn from(diff: &RecipeDiff) -> Self {
        let mut summary = Self {
            recipe_schema_changed: diff.schema_version.is_some(),
            input_settings_changed: diff.input_settings.is_some(),
            layers_added: diff.added_layers.len(),
            layers_removed: diff.removed_layers.len(),
            layers_moved: diff.moved_layers.len(),
            layers_modified: diff.modified_layers.len(),
            ..Self::default()
        };

        for layer in &diff.modified_layers {
            summary.layer_instance_fields_changed += layer.instance.changed_field_count();
            match layer.content.as_ref() {
                Some(LayerContentDiff::InlineGraph { graph }) => {
                    summary.graph_schemas_changed += usize::from(graph.schema_version.is_some());
                    summary.graph_inputs_changed += usize::from(graph.input_types.is_some());
                    summary.graph_outputs_changed += usize::from(graph.output_node.is_some());
                    summary.nodes_added += graph.added_nodes.len();
                    summary.nodes_removed += graph.removed_nodes.len();
                    summary.nodes_modified += graph.modified_nodes.len();
                    for node in &graph.modified_nodes {
                        summary.operation_contracts_changed +=
                            usize::from(node.operation_contract.is_some());
                        summary.node_inputs_changed += usize::from(node.inputs.is_some());
                        summary.node_parameters_changed += usize::from(node.parameters.is_some());
                        summary.node_masks_changed += usize::from(node.mask.is_some());
                    }
                }
                Some(LayerContentDiff::Shared { shared }) => {
                    summary.shared_layer_targets_changed += usize::from(shared.layer_id.is_some());
                    summary.shared_revision_selectors_changed +=
                        usize::from(shared.revision_selector.is_some());
                }
                Some(LayerContentDiff::Replaced { .. }) => {
                    summary.layer_content_replacements += 1;
                }
                None => {}
            }
        }
        summary
    }
}

impl fmt::Display for RecipeDiffSummary {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut parts = Vec::new();
        if self.recipe_schema_changed {
            parts.push("recipe schema changed".to_owned());
        }
        if self.input_settings_changed {
            parts.push("input settings changed".to_owned());
        }
        push_count(&mut parts, self.layers_added, "layer added", "layers added");
        push_count(
            &mut parts,
            self.layers_removed,
            "layer removed",
            "layers removed",
        );
        push_count(&mut parts, self.layers_moved, "layer moved", "layers moved");
        push_count(
            &mut parts,
            self.layers_modified,
            "layer modified",
            "layers modified",
        );
        push_count(&mut parts, self.nodes_added, "node added", "nodes added");
        push_count(
            &mut parts,
            self.nodes_removed,
            "node removed",
            "nodes removed",
        );
        push_count(
            &mut parts,
            self.nodes_modified,
            "node modified",
            "nodes modified",
        );
        push_count(
            &mut parts,
            self.shared_revision_selectors_changed,
            "shared revision changed",
            "shared revisions changed",
        );
        push_count(
            &mut parts,
            self.layer_content_replacements,
            "layer content replaced",
            "layer contents replaced",
        );
        if parts.is_empty() {
            formatter.write_str("no recipe changes")
        } else {
            formatter.write_str(&parts.join(" · "))
        }
    }
}

fn push_count(parts: &mut Vec<String>, count: usize, singular: &str, plural: &str) {
    if count != 0 {
        parts.push(format!(
            "{count} {}",
            if count == 1 { singular } else { plural }
        ));
    }
}
