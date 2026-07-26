//! Semantic version summaries for the immutable development Recipe history.
//!
//! This stays independent from Catalog persistence: it compares already-loaded
//! commits and translates the stable v1 Recipe subset into the desktop ABI.

use super::*;
use shadow_domain::{LayerContentDiff, RecipeDiff};

pub(super) fn commit_record(
    commits: &[RecipeCommitRecord],
    commit_id: RecipeCommitId,
) -> AnyResult<&RecipeCommitRecord> {
    commits
        .iter()
        .find(|record| record.commit.id() == commit_id)
        .ok_or_else(|| anyhow!("Recipe commit {commit_id} does not belong to this photo"))
}

pub(super) fn ffi_edit_version(
    record: &RecipeCommitRecord,
    commits: &[RecipeCommitRecord],
    working_id: Option<RecipeCommitId>,
) -> AnyResult<ffi::FfiEditVersion> {
    let diff = edit_version_diff(record, commits)?;
    Ok(ffi::FfiEditVersion {
        commit_id: record.commit.id().to_string(),
        name: record
            .commit
            .message()
            .unwrap_or("Untitled version")
            .to_owned(),
        created_at_ms: record.commit.created_at_ms(),
        parent_commit_ids: record
            .commit
            .parents()
            .iter()
            .map(ToString::to_string)
            .collect(),
        is_working: working_id == Some(record.commit.id()),
        is_root: diff.is_root,
        recipe_schema_changed: diff.recipe_schema_changed,
        grade_nodes_added: diff.grade_nodes_added,
        grade_nodes_removed: diff.grade_nodes_removed,
        grade_nodes_moved: diff.grade_nodes_moved,
        grade_nodes_modified: diff.grade_nodes_modified,
        render_ops_added: diff.render_ops_added,
        render_ops_removed: diff.render_ops_removed,
        render_ops_modified: diff.render_ops_modified,
        render_op_parameter_blocks_changed: diff.render_op_parameter_blocks_changed,
        changed_basic_parameter_count: checked_count(
            record.commit.id(),
            "changed_basic_parameters",
            diff.changed_basic_parameters.len(),
        )?,
        changed_basic_parameters: diff.changed_basic_parameters,
        has_other_changes: diff.has_other_changes,
    })
}

#[derive(Debug, thiserror::Error)]
pub(super) enum EditVersionDiffError {
    #[error(
        "edit_version_diff.parent_missing: commit {commit_id} references unavailable first parent {parent_id}"
    )]
    ParentMissing {
        commit_id: RecipeCommitId,
        parent_id: RecipeCommitId,
    },
    #[error(
        "edit_version_diff.recipe_mismatch: commit {commit_id} and first parent {parent_id} have different Recipe identities"
    )]
    RecipeMismatch {
        commit_id: RecipeCommitId,
        parent_id: RecipeCommitId,
    },
    #[error(
        "edit_version_diff.count_overflow: {field} for commit {commit_id} exceeds the desktop ABI limit"
    )]
    CountOverflow {
        commit_id: RecipeCommitId,
        field: &'static str,
    },
}

#[derive(Debug, Default)]
pub(super) struct EditVersionDiff {
    pub(super) is_root: bool,
    pub(super) recipe_schema_changed: bool,
    pub(super) grade_nodes_added: u32,
    pub(super) grade_nodes_removed: u32,
    pub(super) grade_nodes_moved: u32,
    pub(super) grade_nodes_modified: u32,
    pub(super) render_ops_added: u32,
    pub(super) render_ops_removed: u32,
    pub(super) render_ops_modified: u32,
    pub(super) render_op_parameter_blocks_changed: u32,
    pub(super) changed_basic_parameters: Vec<String>,
    pub(super) has_other_changes: bool,
}

pub(super) fn edit_version_diff(
    record: &RecipeCommitRecord,
    commits: &[RecipeCommitRecord],
) -> Result<EditVersionDiff, EditVersionDiffError> {
    let Some(parent_id) = record.commit.parents().first().copied() else {
        return Ok(EditVersionDiff {
            is_root: true,
            ..EditVersionDiff::default()
        });
    };
    let parent = commits
        .iter()
        .find(|candidate| candidate.commit.id() == parent_id)
        .ok_or(EditVersionDiffError::ParentMissing {
            commit_id: record.commit.id(),
            parent_id,
        })?;
    if parent.commit.recipe_id() != record.commit.recipe_id() {
        return Err(EditVersionDiffError::RecipeMismatch {
            commit_id: record.commit.id(),
            parent_id,
        });
    }

    let structural = diff_recipe_snapshots(parent.commit.snapshot(), record.commit.snapshot());
    let summary = structural.summary();
    let (changed_basic_parameters, basic_subset_supported) = match (
        decode_grade_stack_draft_from_recipe_v1_snapshot(parent.commit.snapshot()),
        decode_grade_stack_draft_from_recipe_v1_snapshot(record.commit.snapshot()),
    ) {
        (Ok(before), Ok(after)) => (changed_grade_parameters_recipe_v1(&before, &after), true),
        _ => (Vec::new(), false),
    };

    Ok(EditVersionDiff {
        is_root: false,
        recipe_schema_changed: summary.recipe_schema_changed,
        grade_nodes_added: checked_summary_count(
            record.commit.id(),
            "grade_nodes_added",
            summary.layers_added,
        )?,
        grade_nodes_removed: checked_summary_count(
            record.commit.id(),
            "grade_nodes_removed",
            summary.layers_removed,
        )?,
        grade_nodes_moved: checked_summary_count(
            record.commit.id(),
            "grade_nodes_moved",
            summary.layers_moved,
        )?,
        grade_nodes_modified: checked_summary_count(
            record.commit.id(),
            "grade_nodes_modified",
            summary.layers_modified,
        )?,
        render_ops_added: checked_summary_count(
            record.commit.id(),
            "render_ops_added",
            summary.nodes_added,
        )?,
        render_ops_removed: checked_summary_count(
            record.commit.id(),
            "render_ops_removed",
            summary.nodes_removed,
        )?,
        render_ops_modified: checked_summary_count(
            record.commit.id(),
            "render_ops_modified",
            summary.nodes_modified,
        )?,
        render_op_parameter_blocks_changed: checked_summary_count(
            record.commit.id(),
            "render_op_parameter_blocks_changed",
            summary.node_parameters_changed,
        )?,
        has_other_changes: !basic_subset_supported
            || has_other_recipe_changes(
                &structural,
                parent.commit.snapshot(),
                record.commit.snapshot(),
            ),
        changed_basic_parameters,
    })
}

fn checked_summary_count(
    commit_id: RecipeCommitId,
    field: &'static str,
    count: usize,
) -> Result<u32, EditVersionDiffError> {
    checked_count(commit_id, field, count)
}

fn checked_count(
    commit_id: RecipeCommitId,
    field: &'static str,
    count: usize,
) -> Result<u32, EditVersionDiffError> {
    u32::try_from(count).map_err(|_| EditVersionDiffError::CountOverflow { commit_id, field })
}

fn changed_basic_parameters(
    before: BasicEditParameters,
    after: BasicEditParameters,
) -> Vec<String> {
    let mut changed = Vec::new();
    if persisted_float_changed(before.exposure_stops, after.exposure_stops) {
        changed.push("exposure_stops".to_owned());
    }
    if persisted_float_changed(before.contrast_factor, after.contrast_factor) {
        changed.push("contrast_factor".to_owned());
    }
    for (key, before, after) in [
        (
            "white_balance_temperature",
            before.white_balance_temperature,
            after.white_balance_temperature,
        ),
        (
            "white_balance_tint",
            before.white_balance_tint,
            after.white_balance_tint,
        ),
    ] {
        if persisted_float_changed(before, after) {
            changed.push(key.to_owned());
        }
    }
    if persisted_float_changed(before.saturation_factor, after.saturation_factor) {
        changed.push("saturation_factor".to_owned());
    }
    changed
}

fn changed_fine_parameters(before: &FineEditParameters, after: &FineEditParameters) -> Vec<String> {
    let mut changed = Vec::new();
    for (key, before, after) in [
        (
            "highlights",
            before.selective_tone.highlights,
            after.selective_tone.highlights,
        ),
        (
            "shadows",
            before.selective_tone.shadows,
            after.selective_tone.shadows,
        ),
        (
            "whites",
            before.selective_tone.whites,
            after.selective_tone.whites,
        ),
        (
            "blacks",
            before.selective_tone.blacks,
            after.selective_tone.blacks,
        ),
        (
            "vibrance",
            before.perceptual_color.vibrance,
            after.perceptual_color.vibrance,
        ),
    ] {
        if persisted_float_changed(before, after) {
            changed.push(key.to_owned());
        }
    }
    if persisted_array_changed(
        before.perceptual_color.hue_shifts,
        after.perceptual_color.hue_shifts,
    ) {
        changed.push("color_mixer_hue".to_owned());
    }
    if persisted_array_changed(
        before.perceptual_color.saturation,
        after.perceptual_color.saturation,
    ) {
        changed.push("color_mixer_saturation".to_owned());
    }
    if persisted_array_changed(
        before.perceptual_color.lightness,
        after.perceptual_color.lightness,
    ) {
        changed.push("color_mixer_lightness".to_owned());
    }
    if before.perceptual_color.color_range != after.perceptual_color.color_range {
        changed.push("color_range".to_owned());
    }
    if before.perceptual_color.selective_color_relative
        != after.perceptual_color.selective_color_relative
        || before.perceptual_color.selective_color_lightness_protection
            != after.perceptual_color.selective_color_lightness_protection
        || persisted_array_changed(
            before.perceptual_color.selective_color_cmyk,
            after.perceptual_color.selective_color_cmyk,
        )
    {
        changed.push("selective_color".to_owned());
    }
    if before.oklab_lightness_curve != after.oklab_lightness_curve {
        changed.push("oklab_lightness_curve".to_owned());
    }
    if before.oklab_color_warper != after.oklab_color_warper {
        changed.push("color_warper".to_owned());
    }
    if before.lut != after.lut {
        changed.push("lut".to_owned());
    }
    if before.sharpen != after.sharpen {
        changed.push("sharpening".to_owned());
    }
    changed
}

fn persisted_array_changed<const N: usize>(before: [f64; N], after: [f64; N]) -> bool {
    before
        .into_iter()
        .zip(after)
        .any(|(before, after)| persisted_float_changed(before, after))
}

pub(super) fn changed_grade_parameters_recipe_v1(
    before: &GradeStackDraft,
    after: &GradeStackDraft,
) -> Vec<String> {
    let before_by_id = before
        .grade_nodes
        .iter()
        .map(|grade_node| (grade_node.recipe_v1_identity.grade_node_id, grade_node))
        .collect::<HashMap<_, _>>();
    let mut changed = HashSet::new();
    if before.optics != after.optics {
        changed.insert("optics".to_owned());
    }
    for after_grade_node in &after.grade_nodes {
        let Some(before_grade_node) =
            before_by_id.get(&after_grade_node.recipe_v1_identity.grade_node_id)
        else {
            continue;
        };
        changed.extend(changed_basic_parameters(
            before_grade_node.basic,
            after_grade_node.basic,
        ));
        changed.extend(changed_fine_parameters(
            &before_grade_node.fine,
            &after_grade_node.fine,
        ));
        if before_grade_node.enabled != after_grade_node.enabled {
            changed.insert("grade_node_enabled".to_owned());
        }
    }
    [
        "exposure_stops",
        "contrast_factor",
        "white_balance_temperature",
        "white_balance_tint",
        "saturation_factor",
        "grade_node_enabled",
        "oklab_lightness_curve",
        "color_warper",
        "highlights",
        "shadows",
        "whites",
        "blacks",
        "vibrance",
        "color_mixer_hue",
        "color_mixer_saturation",
        "color_mixer_lightness",
        "color_range",
        "selective_color",
        "lut",
        "sharpening",
        "optics",
    ]
    .into_iter()
    .filter(|key| changed.contains(*key))
    .map(str::to_owned)
    .collect()
}

const fn persisted_float_changed(before: f64, after: f64) -> bool {
    before.to_bits() != after.to_bits()
}

/// A basic-parameter-only edit still appears as one modified layer and one or
/// more modified nodes in the generic summary. Inspect the exact diff so the
/// UI can distinguish those container changes from topology/mask/contract
/// changes that its localized basic-control labels do not describe.
pub(super) fn has_other_recipe_changes(
    diff: &RecipeDiff,
    before: &RecipeSnapshot,
    after: &RecipeSnapshot,
) -> bool {
    if diff.schema_version().is_some()
        || !diff.added_layers().is_empty()
        || !diff.removed_layers().is_empty()
        || !diff.moved_layers().is_empty()
    {
        return true;
    }

    if canonical_grade_stack_recipe_v1_identity_is_preserved(before, after) {
        return false;
    }

    diff.modified_layers().iter().any(|layer| {
        if !layer.instance().is_empty() {
            return true;
        }
        match layer.content() {
            Some(LayerContentDiff::InlineGraph { graph }) => {
                graph.schema_version().is_some()
                    || graph.input_types().is_some()
                    || graph.output_node().is_some()
                    || !graph.added_nodes().is_empty()
                    || !graph.removed_nodes().is_empty()
                    || graph.modified_nodes().iter().any(|node| {
                        node.operation_contract().is_some()
                            || node.inputs().is_some()
                            || node.mask().is_some()
                            || node.parameters().is_none()
                            || !node_parameter_change_has_basic_label(after, node.node_id())
                    })
            }
            Some(LayerContentDiff::Shared { .. } | LayerContentDiff::Replaced { .. }) => true,
            None => false,
        }
    })
}

fn canonical_grade_stack_recipe_v1_identity_is_preserved(
    before: &RecipeSnapshot,
    after: &RecipeSnapshot,
) -> bool {
    if before.layers().len() != after.layers().len() {
        return false;
    }
    before
        .layers()
        .iter()
        .zip(after.layers())
        .all(|(before_layer, after_layer)| {
            let (Ok(before_nodes), Ok(after_nodes)) = (
                grade_node_recipe_v1_render_ops(before_layer),
                grade_node_recipe_v1_render_ops(after_layer),
            ) else {
                return false;
            };
            before_layer.id() == after_layer.id()
                && before_layer.label() == after_layer.label()
                && before_nodes.exposure.id() == after_nodes.exposure.id()
                && before_nodes.contrast.id() == after_nodes.contrast.id()
                && before_nodes.white_balance.id() == after_nodes.white_balance.id()
                && before_nodes.saturation.id() == after_nodes.saturation.id()
                && before_nodes.selective_tone.id() == after_nodes.selective_tone.id()
                && before_nodes.perceptual_color.id() == after_nodes.perceptual_color.id()
                && before_nodes.technical_detail.id() == after_nodes.technical_detail.id()
                && before_nodes.color_grading.id() == after_nodes.color_grading.id()
                && before_nodes.lut.id() == after_nodes.lut.id()
                && before_nodes.finishing_effects.id() == after_nodes.finishing_effects.id()
                && match (
                    before_nodes.oklab_lightness_curve,
                    after_nodes.oklab_lightness_curve,
                ) {
                    (Some(before), Some(after)) => before.id() == after.id(),
                    _ => true,
                }
                && match (
                    before_nodes.oklab_color_warper,
                    after_nodes.oklab_color_warper,
                ) {
                    (Some(before), Some(after)) => before.id() == after.id(),
                    _ => true,
                }
        })
}

fn node_parameter_change_has_basic_label(snapshot: &RecipeSnapshot, node_id: NodeId) -> bool {
    snapshot.layers().iter().any(|layer| {
        let LayerContent::Inline { graph } = layer.content() else {
            return false;
        };
        graph.nodes().iter().any(|node| {
            node.id() == node_id
                && matches!(
                    node.operation().operation_id().as_str(),
                    EXPOSURE_OPERATION_ID
                        | CONTRAST_OPERATION_ID
                        | OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID
                        | RGB_WHITE_BALANCE_OPERATION_ID
                        | SATURATION_OPERATION_ID
                        | SELECTIVE_TONE_OPERATION_ID
                        | PERCEPTUAL_COLOR_OPERATION_ID
                        | TECHNICAL_DETAIL_OPERATION_ID
                        | COLOR_GRADING_OPERATION_ID
                        | FINISHING_EFFECTS_OPERATION_ID
                )
        })
    })
}
